// Force Windows Vista+ API availability (Required for Condition Variables)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <malloc.h>
#include <immintrin.h> // AVX2
#include "version.h"

// ==========================================
// CONFIGURATION
// ==========================================
#define MAX_PATH_LEN 4096
#define THREAD_COUNT 16
#define INITIAL_RESULT_CAPACITY 4096
#define WORKER_BATCH_SIZE 64 

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
uint64_t *file_sizes = NULL; // SoA: 32-byte aligned
volatile long result_count = 0;
long result_capacity = 0;
CRITICAL_SECTION result_lock;

// Filter State
int is_filtering = 0;
char filter_text[256] = {0};
int filter_mode = 0; // 0 = Name, 1 = Path
long *filtered_indices = NULL;
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

// Forward Declarations
void open_selection();
void update_filter(int reset_selection);

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

__forceinline int avx2_strcasestr(const char *haystack, const char *needle, size_t needle_len) {
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
// STORAGE & BATCHING
// ==========================================
void add_results_batch(const char (*paths)[MAX_PATH_LEN], const uint64_t *sizes, int count) {
    if (count == 0) return;

    EnterCriticalSection(&result_lock);
    
    if (result_count + count >= result_capacity) {
        long new_cap = result_capacity + count + (result_capacity / 2) + 1024;
        Result *new_ptr = (Result*)realloc(results, new_cap * sizeof(Result));
        uint64_t *new_sizes = (uint64_t*)_aligned_malloc(new_cap * sizeof(uint64_t), 32);
        
        if (new_ptr && new_sizes) {
            results = new_ptr;
            if (file_sizes) {
                memcpy(new_sizes, file_sizes, result_count * sizeof(uint64_t));
                _aligned_free(file_sizes);
            }
            file_sizes = new_sizes;
            result_capacity = new_cap;
        } else {
            if (new_sizes) _aligned_free(new_sizes);
            LeaveCriticalSection(&result_lock);
            return;
        }
    }

    for(int i=0; i<count; i++) {
        strcpy(results[result_count + i].path, paths[i]);
    }
    memcpy(&file_sizes[result_count], sizes, count * sizeof(uint64_t));
    
    result_count += count;
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
// FILTER LOGIC
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
    size_t f_len = strlen(filter_text);

    if (f_len == 0) {
        long i = 0;
        for (; i <= result_count - 8; i += 8) {
            filtered_indices[i] = i; filtered_indices[i+1] = i+1;
            filtered_indices[i+2] = i+2; filtered_indices[i+3] = i+3;
            filtered_indices[i+4] = i+4; filtered_indices[i+5] = i+5;
            filtered_indices[i+6] = i+6; filtered_indices[i+7] = i+7;
        }
        for (; i < result_count; i++) filtered_indices[i] = i;
        filtered_count = result_count;
    } else {
        long i = 0;
        for (; i <= result_count - 4; i += 4) {
            const char *s[4];
            s[0] = results[i].path; s[1] = results[i+1].path;
            s[2] = results[i+2].path; s[3] = results[i+3].path;

            if (filter_mode == 0) {
                const char *ls;
                ls = strrchr(s[0], '\\'); if(ls) s[0] = ls+1;
                ls = strrchr(s[1], '\\'); if(ls) s[1] = ls+1;
                ls = strrchr(s[2], '\\'); if(ls) s[2] = ls+1;
                ls = strrchr(s[3], '\\'); if(ls) s[3] = ls+1;
            }

            int m[4];
            if (has_wildcard) {
                m[0] = fast_glob_match(s[0], filter_text); m[1] = fast_glob_match(s[1], filter_text);
                m[2] = fast_glob_match(s[2], filter_text); m[3] = fast_glob_match(s[3], filter_text);
            } else {
                m[0] = (stristr(s[0], filter_text) != NULL); m[1] = (stristr(s[1], filter_text) != NULL);
                m[2] = (stristr(s[2], filter_text) != NULL); m[3] = (stristr(s[3], filter_text) != NULL);
            }

            if (m[0]) filtered_indices[filtered_count++] = i;
            if (m[1]) filtered_indices[filtered_count++] = i+1;
            if (m[2]) filtered_indices[filtered_count++] = i+2;
            if (m[3]) filtered_indices[filtered_count++] = i+3;
        }

        for (; i < result_count; i++) {
            const char *s = results[i].path;
            if (filter_mode == 0) {
                const char *ls = strrchr(s, '\\');
                if (ls) s = ls + 1;
            }
            int match = has_wildcard ? fast_glob_match(s, filter_text) : (stristr(s, filter_text) != NULL);
            if (match) filtered_indices[filtered_count++] = i;
        }
    }
    
    if (reset_selection) {
        selected_index = 0;
        scroll_offset = 0;
    } else {
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
CONDITION_VARIABLE queue_cond;
volatile long idle_workers = 0;

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
    WakeConditionVariable(&queue_cond);
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
// AVX2 SIZE ESTIMATOR
// ==========================================
unsigned long long calculate_total_size_avx2(uint64_t *sizes, long *indices, long count) {
    if (count == 0) return 0;
    __m256i v_sum = _mm256_setzero_si256();
    long i = 0;
    
    if (indices) {
        for (; i <= count - 8; i += 8) {
            __m256i v_idx_raw = _mm256_loadu_si256((__m256i const*)&indices[i]);
            __m256i v_sizes_lo = _mm256_i32gather_epi64((long long const*)sizes,
                                                       _mm256_castsi256_si128(v_idx_raw), 8);
            __m128i v_idx_hi_128 = _mm256_extracti128_si256(v_idx_raw, 1);
            __m256i v_sizes_hi = _mm256_i32gather_epi64((long long const*)sizes, v_idx_hi_128, 8);
            v_sum = _mm256_add_epi64(v_sum, v_sizes_lo);
            v_sum = _mm256_add_epi64(v_sum, v_sizes_hi);
        }
        unsigned long long total = 0;
        uint64_t buffer[4];
        _mm256_storeu_si256((__m256i*)buffer, v_sum);
        total = buffer[0] + buffer[1] + buffer[2] + buffer[3];
        for (; i < count; i++) total += sizes[indices[i]];
        return total;
    } else {
        for (; i <= count - 16; i += 16) {
            __m256i v0 = _mm256_load_si256((__m256i const*)&sizes[i]);
            __m256i v1 = _mm256_load_si256((__m256i const*)&sizes[i+4]);
            __m256i v2 = _mm256_load_si256((__m256i const*)&sizes[i+8]);
            __m256i v3 = _mm256_load_si256((__m256i const*)&sizes[i+12]);
            __m256i sum01 = _mm256_add_epi64(v0, v1);
            __m256i sum23 = _mm256_add_epi64(v2, v3);
            v_sum = _mm256_add_epi64(v_sum, _mm256_add_epi64(sum01, sum23));
        }
        unsigned long long total = 0;
        uint64_t buffer[4];
        _mm256_storeu_si256((__m256i*)buffer, v_sum);
        total = buffer[0] + buffer[1] + buffer[2] + buffer[3];
        for (; i < count; i++) total += sizes[i];
        return total;
    }
}

void format_size_fast(unsigned long long bytes, char *out) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = (double)bytes;
    if (bytes > 1024) { size /= 1024; unit_idx++; }
    if (size > 1024) { size /= 1024; unit_idx++; }
    if (size > 1024) { size /= 1024; unit_idx++; }
    if (size > 1024) { size /= 1024; unit_idx++; }
    snprintf(out, 32, "%.2f %s", size, units[unit_idx]);
}

// ==========================================
// WORKER THREAD (ADAPTIVE BATCHING)
// ==========================================
unsigned __stdcall worker_thread(void *arg) {
    char current_dir[MAX_PATH_LEN];
    char search_path[MAX_PATH_LEN];
    
    // THREAD LOCAL BATCH STORAGE
    char (*batch_paths)[MAX_PATH_LEN] = malloc(WORKER_BATCH_SIZE * MAX_PATH_LEN);
    uint64_t *batch_sizes = malloc(WORKER_BATCH_SIZE * sizeof(uint64_t));
    int batch_count = 0;
    
    // ADAPTIVE BATCHING: Start at 1 for instant feedback, ramp to 64 for speed
    int current_batch_limit = 1;

    WIN32_FIND_DATAA find_data;
    HANDLE hFind;

    InterlockedIncrement(&active_workers);

    while (running) {
        int has_work = 0;

        // ----------------------------------------
        // THE WAITING ROOM
        // ----------------------------------------
        EnterCriticalSection(&queue_lock);
        while (q_head == NULL && running) {
            if (batch_count > 0) {
                LeaveCriticalSection(&queue_lock); 
                add_results_batch(batch_paths, batch_sizes, batch_count);
                batch_count = 0;
                EnterCriticalSection(&queue_lock); 
                continue; 
            }

            // If we sleep, reset batch limit to 1 for responsiveness on next wake
            current_batch_limit = 1;

            idle_workers++;
            if (idle_workers == THREAD_COUNT) {
                finished_scanning = 1;
                WakeAllConditionVariable(&queue_cond);
            }
            if (finished_scanning) {
                idle_workers--;
                LeaveCriticalSection(&queue_lock);
                if (batch_count > 0) add_results_batch(batch_paths, batch_sizes, batch_count);
                free(batch_paths);
                free(batch_sizes);
                InterlockedDecrement(&active_workers);
                return 0;
            }
            SleepConditionVariableCS(&queue_cond, &queue_lock, INFINITE);
            idle_workers--;
        }

        if (q_head) {
            QueueNode *node = q_head;
            strcpy(current_dir, node->path);
            q_head = node->next;
            if (q_head == NULL) q_tail = NULL;
            free(node->path);
            free(node);
            has_work = 1;
        }
        LeaveCriticalSection(&queue_lock);

        // ----------------------------------------
        // THE GRIND
        // ----------------------------------------
        if (has_work) {
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
                        join_path(batch_paths[batch_count], current_dir, find_data.cFileName);
                        batch_sizes[batch_count] = ((uint64_t)find_data.nFileSizeHigh << 32) | find_data.nFileSizeLow;
                        batch_count++;

                        // ADAPTIVE FLUSH TRIGGER
                        if (batch_count >= current_batch_limit) {
                            add_results_batch(batch_paths, batch_sizes, batch_count);
                            batch_count = 0;
                            
                            // Ramp up: 1 -> 8 -> 64
                            if (current_batch_limit < WORKER_BATCH_SIZE) {
                                current_batch_limit *= 8;
                                if (current_batch_limit > WORKER_BATCH_SIZE) 
                                    current_batch_limit = WORKER_BATCH_SIZE;
                            }
                        }
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
        }
    }
    
    if (batch_count > 0) add_results_batch(batch_paths, batch_sizes, batch_count);
    free(batch_paths);
    free(batch_sizes);
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

    for (int i = 0; i < needed_size; i++) {
        buffer[i].Char.AsciiChar = ' ';
        buffer[i].Attributes = FOREGROUND_WHITE;
    }

    char header[512];
    long display_total = is_filtering ? filtered_count : result_count;
    unsigned long long selected_bytes = 0;
    int have_selected_size = 0;
    
    EnterCriticalSection(&result_lock);
    long count = is_filtering ? filtered_count : result_count;
    unsigned long long total_view_bytes = calculate_total_size_avx2(file_sizes, is_filtering ? filtered_indices : NULL, count);
    
    if (count > 0 && selected_index >= 0 && selected_index < count) {
        long real_index_hdr = is_filtering ? filtered_indices[selected_index] : selected_index;
        if (file_sizes) { selected_bytes = file_sizes[real_index_hdr]; have_selected_size = 1; }
    }
    LeaveCriticalSection(&result_lock);

    char sel_size_str[32] = "-";
    if (have_selected_size) format_size_fast(selected_bytes, sel_size_str);
    
    char total_size_str[32] = "0 B";
    format_size_fast(total_view_bytes, total_size_str);

    snprintf(header, 512, " blade %s :: Found: %ld (%s) :: Sel: %s :: %s", 
             VERSION, display_total, total_size_str, sel_size_str,
             finished_scanning ? "Ready" : "Scanning...");
    
    for (int i = 0; i < strlen(header) && i < console_width; i++) {
        buffer[i].Char.AsciiChar = header[i];
        buffer[i].Attributes = BACKGROUND_RED | FOREGROUND_INTENSITY | FOREGROUND_WHITE;
    }

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

    EnterCriticalSection(&result_lock);
    count = is_filtering ? filtered_count : result_count;
    
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
    file_sizes = (uint64_t*)_aligned_malloc(INITIAL_RESULT_CAPACITY * sizeof(uint64_t), 32);
    result_capacity = INITIAL_RESULT_CAPACITY;
    filtered_indices = (long*)malloc(INITIAL_RESULT_CAPACITY * sizeof(long));
    filtered_capacity = INITIAL_RESULT_CAPACITY;

    InitializeCriticalSection(&queue_lock);
    InitializeCriticalSection(&result_lock);
    InitializeConditionVariable(&queue_cond); 

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

    char start_dir[MAX_PATH_LEN];
    char *file_part;
    DWORD result_len = GetFullPathNameA(argv[1], MAX_PATH_LEN, start_dir, &file_part);
    if (result_len == 0) strcpy(start_dir, argv[1]);
    else {
        size_t len = strlen(start_dir);
        if (len > 3 && start_dir[len - 1] == '\\') start_dir[len - 1] = '\0';
    }

    push_job(start_dir);

    HANDLE threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        threads[i] = (HANDLE)_beginthreadex(NULL, 0, worker_thread, NULL, 0, NULL);
    }

    INPUT_RECORD ir[128];
    DWORD recordsRead;

    while (running) {
        if (is_filtering && !finished_scanning) update_filter(0);

        DWORD events = 0;
        GetNumberOfConsoleInputEvents(hConsoleIn, &events);
        
        if (events > 0) {
            ReadConsoleInput(hConsoleIn, ir, 128, &recordsRead);
            for (DWORD i = 0; i < recordsRead; i++) {
                if (ir[i].EventType == KEY_EVENT && ir[i].Event.KeyEvent.bKeyDown) {
                    WORD vk = ir[i].Event.KeyEvent.wVirtualKeyCode;
                    char ascii = ir[i].Event.KeyEvent.uChar.AsciiChar;
                    DWORD ctrl = ir[i].Event.KeyEvent.dwControlKeyState;

                    if (vk == 'F' && (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
                        is_filtering = !is_filtering;
                        if (is_filtering) update_filter(1);
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

                    long max_items = is_filtering ? filtered_count : result_count;
                    
                    if (vk == VK_UP && selected_index > 0) { selected_index--; continue; }
                    if (vk == VK_DOWN && selected_index < max_items - 1) { selected_index++; continue; }
                    if (vk == VK_PRIOR) { selected_index -= 10; if (selected_index < 0) selected_index = 0; continue; }
                    if (vk == VK_NEXT) { selected_index += 10; if (selected_index >= max_items) selected_index = max_items - 1; continue; }

                    if (is_filtering) {
                        if (vk == VK_TAB) { filter_mode = !filter_mode; update_filter(1); }
                        else if (vk == VK_BACK) {
                            size_t len = strlen(filter_text);
                            if (len > 0) { filter_text[len - 1] = '\0'; update_filter(1); }
                        }
                        else if (isprint((unsigned char)ascii)) {
                            size_t len = strlen(filter_text);
                            if (len < 255) { filter_text[len] = ascii; filter_text[len + 1] = '\0'; update_filter(1); }
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