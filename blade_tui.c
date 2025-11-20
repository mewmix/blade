#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <immintrin.h> // AVX2
#include "version.h"

// ==========================================
// CONFIGURATION
// ==========================================
#define MAX_PATH_LEN 4096
#define THREAD_COUNT 16
#define INITIAL_RESULT_CAPACITY 4096

// Color Macros
#define FOREGROUND_WHITE (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define FOREGROUND_BLACK 0

// ==========================================
// GLOBAL STATE
// ==========================================
typedef struct {
    char path[MAX_PATH_LEN];
} Result;

Result *results = NULL;
volatile long result_count = 0;
long result_capacity = 0;
CRITICAL_SECTION result_lock;

// Filter State
int is_filtering = 0;
char filter_text[256] = {0};
int filter_mode = 0; // 0 = Name, 1 = Path
long *filtered_indices = NULL; // Pointer to indices in 'results'
long filtered_count = 0;
long filtered_capacity = 0;

// UI State
int selected_index = 0;
int scroll_offset = 0;
int console_width = 80;
int console_height = 25;
HANDLE hConsoleOut;
HANDLE hConsoleIn;
volatile int running = 1;

// Search State
volatile long active_workers = 0;
volatile long finished_scanning = 0;
char TARGET_RAW[256];
char TARGET_LOWER[256];
size_t TARGET_LEN;
int IS_WILDCARD = 0;

// ==========================================
// SEARCH ENGINES
// ==========================================
// Glob Matcher (Wildcards: *, ?) - Case Insensitive
int fast_glob_match(const char *text, const char *pattern) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1;
            while (*text) {
                if (fast_glob_match(text, pattern)) return 1;
                text++;
            }
            return 0;
        }
        char t = tolower((unsigned char)*text);
        char p = tolower((unsigned char)*pattern);
        if (t != p && *pattern != '?') return 0;
        if (!*text) return 0;
        text++; pattern++;
    }
    return !*text;
}

// Substring Matcher - Case Insensitive
const char* stristr(const char* haystack, const char* needle) {
    if (!*needle) return haystack;
    for (; *haystack; ++haystack) {
        if (tolower(*haystack) == tolower(*needle)) {
            const char *h, *n;
            for (h = haystack, n = needle; *h && *n; ++h, ++n) {
                if (tolower(*h) != tolower(*n)) break;
            }
            if (!*n) return haystack;
        }
    }
    return NULL;
}

// AVX2 Substring Matcher (For main search)
int avx2_strcasestr(const char *haystack, const char *needle, size_t needle_len) {
    if (needle_len == 0) return 1;
    char first_char = tolower(needle[0]);
    __m256i vec_first = _mm256_set1_epi8(first_char);
    __m256i vec_case_mask = _mm256_set1_epi8(0x20);
    size_t i = 0;
    for (; haystack[i]; i += 32) {
        __m256i block = _mm256_loadu_si256((const __m256i*)(haystack + i));
        __m256i block_lower = _mm256_or_si256(block, vec_case_mask);
        __m256i eq = _mm256_cmpeq_epi8(vec_first, block_lower);
        unsigned int mask = _mm256_movemask_epi8(eq);
        while (mask) {
            int bit_pos = __builtin_ctz(mask);
            if (_strnicmp(haystack + i + bit_pos, needle, needle_len) == 0) return 1;
            mask &= ~(1 << bit_pos);
        }
    }
    return 0;
}

// ==========================================
// HELPERS
// ==========================================
void add_result(const char *path) {
    EnterCriticalSection(&result_lock);
    if (result_count >= result_capacity) {
        long new_cap = result_capacity + (result_capacity / 2) + 1024;
        Result *new_ptr = (Result*)realloc(results, new_cap * sizeof(Result));
        if (new_ptr) {
            results = new_ptr;
            result_capacity = new_cap;
        } else {
            LeaveCriticalSection(&result_lock);
            return; 
        }
    }
    strcpy(results[result_count].path, path);
    result_count++;
    LeaveCriticalSection(&result_lock);
}

void join_path(char *dest, const char *p1, const char *p2) {
    size_t len = strlen(p1);
    strcpy(dest, p1);
    if (len > 0 && dest[len - 1] != '\\' && dest[len - 1] != '/') {
        strcat(dest, "\\");
    }
    strcat(dest, p2);
}

// ==========================================
// FILTER LOGIC (UPDATED)
// ==========================================
void update_filter(int reset_selection) {
    EnterCriticalSection(&result_lock);
    
    if (filtered_capacity < result_count) {
        long new_cap = result_count + 1024;
        long *new_ptr = (long*)realloc(filtered_indices, new_cap * sizeof(long));
        if (new_ptr) {
            filtered_indices = new_ptr;
            filtered_capacity = new_cap;
        }
    }

    filtered_count = 0;
    int has_wildcard = (strchr(filter_text, '*') || strchr(filter_text, '?'));

    if (strlen(filter_text) == 0) {
        for (long i = 0; i < result_count; i++) filtered_indices[i] = i;
        filtered_count = result_count;
    } else {
        for (long i = 0; i < result_count; i++) {
            const char *search_haystack = results[i].path;
            
            // Filter Mode: 0=Name, 1=Path
            if (filter_mode == 0) {
                const char *last_slash = strrchr(search_haystack, '\\');
                if (last_slash) search_haystack = last_slash + 1;
            }

            // Hybrid Matcher: Use Glob if wildcards exist, else Substring
            int match = 0;
            if (has_wildcard) {
                match = fast_glob_match(search_haystack, filter_text);
            } else {
                match = (stristr(search_haystack, filter_text) != NULL);
            }

            if (match) {
                filtered_indices[filtered_count++] = i;
            }
        }
    }
    
    if (reset_selection) {
        selected_index = 0;
        scroll_offset = 0;
    } else {
        // Clamp if filtered count shrank
        if (filtered_count > 0 && selected_index >= filtered_count) {
            selected_index = filtered_count - 1;
        }
    }
    
    LeaveCriticalSection(&result_lock);
}

// ==========================================
// DYNAMIC WORK QUEUE
// ==========================================
typedef struct QueueNode {
    char *path;
    struct QueueNode *next;
} QueueNode;

QueueNode *q_head = NULL;
QueueNode *q_tail = NULL;
CRITICAL_SECTION queue_lock;

void push_job(const char *path) {
    QueueNode *node = (QueueNode*)malloc(sizeof(QueueNode));
    if (!node) return;
    node->path = _strdup(path);
    node->next = NULL;

    EnterCriticalSection(&queue_lock);
    if (q_tail) {
        q_tail->next = node;
        q_tail = node;
    } else {
        q_head = q_tail = node;
    }
    LeaveCriticalSection(&queue_lock);
}

int pop_job(char *out_path) {
    int found = 0;
    EnterCriticalSection(&queue_lock);
    if (q_head) {
        QueueNode *node = q_head;
        strcpy(out_path, node->path);
        q_head = node->next;
        if (q_head == NULL) q_tail = NULL;
        free(node->path);
        free(node);
        found = 1;
    }
    LeaveCriticalSection(&queue_lock);
    return found;
}

// ==========================================
// WORKER THREAD
// ==========================================
unsigned __stdcall worker_thread(void *arg) {
    char current_dir[MAX_PATH_LEN];
    char search_path[MAX_PATH_LEN];
    char full_path[MAX_PATH_LEN];
    WIN32_FIND_DATAA find_data;
    HANDLE hFind;

    InterlockedIncrement(&active_workers);
    while (running) {
        if (pop_job(current_dir)) {
            join_path(search_path, current_dir, "*");
            hFind = FindFirstFileExA(search_path, FindExInfoBasic, &find_data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (find_data.cFileName[0] == '.') {
                        if (find_data.cFileName[1] == '\0') continue;
                        if (find_data.cFileName[1] == '.' && find_data.cFileName[2] == '\0') continue;
                    }
                    
                    int match = 0;
                    if (IS_WILDCARD) match = fast_glob_match(find_data.cFileName, TARGET_RAW);
                    else match = avx2_strcasestr(find_data.cFileName, TARGET_LOWER, TARGET_LEN);

                    if (match) {
                        join_path(full_path, current_dir, find_data.cFileName);
                        add_result(full_path);
                    }
                    
                    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                            char new_dir_path[MAX_PATH_LEN];
                            join_path(new_dir_path, current_dir, find_data.cFileName);
                            push_job(new_dir_path);
                        }
                    }
                } while (FindNextFileA(hFind, &find_data) && running);
                FindClose(hFind);
            }
        } else {
            EnterCriticalSection(&queue_lock);
            int empty = (q_head == NULL);
            LeaveCriticalSection(&queue_lock);
            if (finished_scanning && empty) break;
            SwitchToThread();
        }
    }
    InterlockedDecrement(&active_workers);
    return 0;
}

// ==========================================
// SIGNAL HANDLER
// ==========================================
BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    if (fdwCtrlType == CTRL_C_EVENT || fdwCtrlType == CTRL_BREAK_EVENT || fdwCtrlType == CTRL_CLOSE_EVENT) {
        running = 0;
        return TRUE;
    }
    return FALSE;
}

// ==========================================
// UI RENDERING
// ==========================================
void render_ui() {
    static CHAR_INFO *buffer = NULL;
    static int buf_size = 0;
    
    int needed_size = console_width * console_height;
    if (!buffer || buf_size != needed_size) {
        if (buffer) free(buffer);
        buffer = (CHAR_INFO*)malloc(needed_size * sizeof(CHAR_INFO));
        buf_size = needed_size;
    }

    // Clear Background
    for (int i = 0; i < needed_size; i++) {
        buffer[i].Char.AsciiChar = ' ';
        buffer[i].Attributes = FOREGROUND_WHITE;
    }

    // Header
    char header[512];
    long display_total = is_filtering ? filtered_count : result_count;
    snprintf(header, 512, " blade %s (%s) :: Search: %s :: Found: %ld%s", 
             VERSION, COMMIT_SHA, TARGET_RAW, display_total, 
             finished_scanning ? "" : " (Scanning...)");
    
    for (int i = 0; i < strlen(header) && i < console_width; i++) {
        buffer[i].Char.AsciiChar = header[i];
        buffer[i].Attributes = BACKGROUND_RED | FOREGROUND_INTENSITY | FOREGROUND_WHITE;
    }

    // Filter Bar
    int list_start_y = 1;
    int list_height = console_height - 1;
    
    if (is_filtering) {
        char filter_bar[512];
        char *mode_str = (filter_mode == 0) ? "Name" : "Path";
        snprintf(filter_bar, 512, " FILTER [%s]: %s_", mode_str, filter_text);
        
        for (int i = 0; i < console_width; i++) {
            int buf_idx = (console_height - 1) * console_width + i;
            if (i < strlen(filter_bar)) {
                buffer[buf_idx].Char.AsciiChar = filter_bar[i];
            } else {
                buffer[buf_idx].Char.AsciiChar = ' ';
            }
            buffer[buf_idx].Attributes = BACKGROUND_BLUE | FOREGROUND_INTENSITY | FOREGROUND_WHITE;
        }
        list_height--; 
    }

    // List
    EnterCriticalSection(&result_lock);
    long count = is_filtering ? filtered_count : result_count;
    
    if (selected_index < scroll_offset) scroll_offset = selected_index;
    if (selected_index >= scroll_offset + (list_height)) scroll_offset = selected_index - (list_height - 1);
    if (scroll_offset < 0) scroll_offset = 0;

    int y = list_start_y;
    for (int i = scroll_offset; i < count && y < (list_start_y + list_height); i++) {
        int is_selected = (i == selected_index);
        WORD attr = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        if (is_selected) attr = BACKGROUND_GREEN | FOREGROUND_BLACK;

        long real_index = is_filtering ? filtered_indices[i] : i;
        char *text = results[real_index].path;

        int len = strlen(text);
        for (int x = 0; x < len && x < console_width; x++) {
            buffer[y * console_width + x].Char.AsciiChar = text[x];
            buffer[y * console_width + x].Attributes = attr;
        }
        y++;
    }
    LeaveCriticalSection(&result_lock);

    COORD bufferSize = { (SHORT)console_width, (SHORT)console_height };
    COORD bufferCoord = { 0, 0 };
    SMALL_RECT writeRegion = { 0, 0, (SHORT)(console_width - 1), (SHORT)(console_height - 1) };
    WriteConsoleOutputA(hConsoleOut, buffer, bufferSize, bufferCoord, &writeRegion);
}

void open_selection() {
    long count = is_filtering ? filtered_count : result_count;
    if (count == 0) return;
    
    long real_index = is_filtering ? filtered_indices[selected_index] : selected_index;
    
    char absolute_path[MAX_PATH_LEN];
    char *file_part;
    
    GetFullPathNameA(results[real_index].path, MAX_PATH_LEN, absolute_path, &file_part);
    for (int i = 0; absolute_path[i]; i++) {
        if (absolute_path[i] == '/') absolute_path[i] = '\\';
    }
    char param[MAX_PATH_LEN + 32];
    snprintf(param, sizeof(param), "/select,\"%s\"", absolute_path);
    ShellExecuteA(NULL, "open", "explorer.exe", param, NULL, SW_SHOWDEFAULT);
}

// ==========================================
// MAIN
// ==========================================
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("version %s (%s)\n", VERSION, COMMIT_SHA);
        printf("Usage: blade.exe <directory> <search_term>\n");
        return 1;
    }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    strcpy(TARGET_RAW, argv[2]);
    TARGET_LEN = strlen(TARGET_RAW);
    if (strchr(TARGET_RAW, '*') || strchr(TARGET_RAW, '?')) IS_WILDCARD = 1;
    for(int i=0; i<TARGET_LEN; i++) TARGET_LOWER[i] = tolower(TARGET_RAW[i]);
    TARGET_LOWER[TARGET_LEN] = 0;

    results = (Result*)malloc(INITIAL_RESULT_CAPACITY * sizeof(Result));
    result_capacity = INITIAL_RESULT_CAPACITY;
    filtered_indices = (long*)malloc(INITIAL_RESULT_CAPACITY * sizeof(long));
    filtered_capacity = INITIAL_RESULT_CAPACITY;

    InitializeCriticalSection(&queue_lock);
    InitializeCriticalSection(&result_lock);

    hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hConsoleIn = GetStdHandle(STD_INPUT_HANDLE);
    
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsoleOut, &cursorInfo);
    cursorInfo.bVisible = FALSE;
    SetConsoleCursorInfo(hConsoleOut, &cursorInfo);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hConsoleOut, &csbi);
    console_width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    console_height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

    // ==========================================
    // PATH RESOLUTION (FIXED)
    // ==========================================
    char start_dir[MAX_PATH_LEN];
    char *file_part;
    
    // Convert to absolute path immediately
    DWORD result_len = GetFullPathNameA(argv[1], MAX_PATH_LEN, start_dir, &file_part);
    if (result_len == 0) {
        strcpy(start_dir, argv[1]);
    } else {
        // Normalize trailing slash logic for directories vs root
        size_t len = strlen(start_dir);
        if (len > 3 && start_dir[len - 1] == '\\') {
            start_dir[len - 1] = '\0';
        }
    }

    push_job(start_dir);

    HANDLE threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        threads[i] = (HANDLE)_beginthreadex(NULL, 0, worker_thread, NULL, 0, NULL);
    }

    INPUT_RECORD ir[128];
    DWORD recordsRead;

    while (running) {
        EnterCriticalSection(&queue_lock);
        int empty = (q_head == NULL);
        LeaveCriticalSection(&queue_lock);

        if (active_workers == 0 && empty) finished_scanning = 1;
        else finished_scanning = 0;
        
        if (is_filtering && !finished_scanning) update_filter(0); // 0 = Don't reset selection

        DWORD events = 0;
        GetNumberOfConsoleInputEvents(hConsoleIn, &events);
        
        if (events > 0) {
            ReadConsoleInput(hConsoleIn, ir, 128, &recordsRead);
            for (DWORD i = 0; i < recordsRead; i++) {
                if (ir[i].EventType == KEY_EVENT && ir[i].Event.KeyEvent.bKeyDown) {
                    WORD vk = ir[i].Event.KeyEvent.wVirtualKeyCode;
                    char ascii = ir[i].Event.KeyEvent.uChar.AsciiChar;
                    DWORD ctrl = ir[i].Event.KeyEvent.dwControlKeyState;

                    // 1. Global Hotkeys
                    if (vk == 'F' && (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
                        is_filtering = !is_filtering;
                        if (is_filtering) update_filter(1); // Reset selection on open
                        continue;
                    }
                    if (vk == VK_ESCAPE) {
                        if (is_filtering) {
                            is_filtering = 0;
                            memset(filter_text, 0, sizeof(filter_text));
                            update_filter(1);
                        } else {
                            running = 0;
                        }
                        continue;
                    }
                    if (vk == VK_RETURN) {
                        open_selection();
                        continue;
                    }

                    // 2. Navigation (Always Active)
                    long max_items = is_filtering ? filtered_count : result_count;
                    
                    if (vk == VK_UP && selected_index > 0) {
                        selected_index--;
                        continue;
                    }
                    if (vk == VK_DOWN && selected_index < max_items - 1) {
                        selected_index++;
                        continue;
                    }
                    if (vk == VK_PRIOR) { // Page Up
                        selected_index -= 10;
                        if (selected_index < 0) selected_index = 0;
                        continue;
                    }
                    if (vk == VK_NEXT) { // Page Down
                        selected_index += 10;
                        if (selected_index >= max_items) selected_index = max_items - 1;
                        continue;
                    }

                    // 3. Filter Text Input (Only if filtering)
                    if (is_filtering) {
                        if (vk == VK_TAB) {
                            filter_mode = !filter_mode; 
                            update_filter(1);
                        }
                        else if (vk == VK_BACK) {
                            size_t len = strlen(filter_text);
                            if (len > 0) {
                                filter_text[len - 1] = '\0';
                                update_filter(1);
                            }
                        }
                        else if (isprint((unsigned char)ascii)) {
                            size_t len = strlen(filter_text);
                            if (len < 255) {
                                filter_text[len] = ascii;
                                filter_text[len + 1] = '\0';
                                update_filter(1);
                            }
                        }
                    }
                }
            }
        }

        render_ui();
        Sleep(16); 
    }

    cursorInfo.bVisible = TRUE;
    SetConsoleCursorInfo(hConsoleOut, &cursorInfo);
    SetConsoleTextAttribute(hConsoleOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    DWORD written;
    COORD coord = {0, 0};
    DWORD size = console_width * console_height;
    FillConsoleOutputCharacterA(hConsoleOut, ' ', size, coord, &written);
    FillConsoleOutputAttribute(hConsoleOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE, size, coord, &written);
    SetConsoleCursorPosition(hConsoleOut, coord);

    return 0;
}