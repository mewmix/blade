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
// SEARCH ENGINES
// ==========================================
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
// DYNAMIC WORK QUEUE (Linked List)
// ==========================================
typedef struct QueueNode {
    char *path;
    struct QueueNode *next;
} QueueNode;

QueueNode *q_head = NULL;
QueueNode *q_tail = NULL;
CRITICAL_SECTION queue_lock;
volatile long queue_size = 0;

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
    queue_size++; // Just for debug/stats if needed
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
        queue_size--;
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
                        // Skip Reparse Points (Junctions) to avoid loops/duplicates
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
            // Check if we are done
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
    char header[256];
    snprintf(header, 256, " blade %s (%s) :: Search: %s :: Found: %ld :: [ARROWS] Nav [ENTER] Open [ESC] Exit", 
             VERSION, COMMIT_SHA, TARGET_RAW, result_count);
    
    for (int i = 0; i < strlen(header) && i < console_width; i++) {
        buffer[i].Char.AsciiChar = header[i];
        buffer[i].Attributes = BACKGROUND_RED | FOREGROUND_INTENSITY | FOREGROUND_WHITE;
    }

    // List
    EnterCriticalSection(&result_lock);
    long count = result_count;
    
    if (selected_index < scroll_offset) scroll_offset = selected_index;
    if (selected_index >= scroll_offset + (console_height - 2)) scroll_offset = selected_index - (console_height - 3);
    if (scroll_offset < 0) scroll_offset = 0;

    int y = 1;
    for (int i = scroll_offset; i < count && y < console_height; i++) {
        int is_selected = (i == selected_index);
        WORD attr = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        if (is_selected) attr = BACKGROUND_GREEN | FOREGROUND_BLACK;

        char *text = results[i].path;
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
    if (result_count == 0) return;
    
    char absolute_path[MAX_PATH_LEN];
    char *file_part;
    
    GetFullPathNameA(results[selected_index].path, MAX_PATH_LEN, absolute_path, &file_part);
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

    // Clean Input Path
    char start_dir[MAX_PATH_LEN];
    strcpy(start_dir, argv[1]);
    size_t start_len = strlen(start_dir);
    if (start_len > 1 && start_dir[start_len-1] == '\\') {
        start_dir[start_len-1] = '\0';
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

        DWORD events = 0;
        GetNumberOfConsoleInputEvents(hConsoleIn, &events);
        
        if (events > 0) {
            ReadConsoleInput(hConsoleIn, ir, 128, &recordsRead);
            for (DWORD i = 0; i < recordsRead; i++) {
                if (ir[i].EventType == KEY_EVENT && ir[i].Event.KeyEvent.bKeyDown) {
                    WORD vk = ir[i].Event.KeyEvent.wVirtualKeyCode;
                    if (vk == VK_ESCAPE) running = 0;
                    else if (vk == VK_UP && selected_index > 0) selected_index--;
                    else if (vk == VK_DOWN && selected_index < result_count - 1) selected_index++;
                    else if (vk == VK_PRIOR) {
                         selected_index -= 10;
                         if (selected_index < 0) selected_index = 0;
                    }
                    else if (vk == VK_NEXT) {
                         selected_index += 10;
                         if (selected_index >= result_count) selected_index = result_count - 1;
                    }
                    else if (vk == VK_RETURN) open_selection();
                }
            }
        }

        render_ui();
        Sleep(16); 
    }

    // Cleanup
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