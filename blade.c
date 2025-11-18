#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h> // AVX2

// ==========================================
// CONFIGURATION
// ==========================================
#define MAX_PATH_LEN 4096 // Windows allows long paths if configured, typical 260
#define THREAD_COUNT 8
#define QUEUE_SIZE 8192 // Larger queue for deep folder structures

// ==========================================
// AVX2 SIMD ENGINE (The same core logic)
// ==========================================
int avx2_strstr(const char *haystack, const char *needle, size_t needle_len) {
    if (needle_len == 0) return 1;
    __m256i first = _mm256_set1_epi8(needle[0]);
    size_t i = 0;
    for (; haystack[i]; i += 32) {
        __m256i block = _mm256_loadu_si256((const __m256i*)(haystack + i));
        __m256i eq_first = _mm256_cmpeq_epi8(first, block);
        unsigned int mask = _mm256_movemask_epi8(eq_first);
        while (mask) {
            int bit_pos = __builtin_ctz(mask);
            if (strncmp(haystack + i + bit_pos, needle, needle_len) == 0) return 1;
            mask &= ~(1 << bit_pos);
        }
    }
    return 0;
}

// ==========================================
// HIGH PERF WORK QUEUE
// ==========================================
typedef struct {
    char path[MAX_PATH_LEN];
} Job;

Job job_queue[QUEUE_SIZE];
volatile long queue_head = 0;
volatile long queue_tail = 0;
volatile long active_workers = 0;
volatile long finished_scanning = 0;

CRITICAL_SECTION queue_lock;

void push_job(const char *path) {
    EnterCriticalSection(&queue_lock);
    // Simple circular buffer; in production, handle overflow
    int idx = queue_tail % QUEUE_SIZE;
    strcpy(job_queue[idx].path, path);
    queue_tail++;
    LeaveCriticalSection(&queue_lock);
}

int pop_job(char *out_path) {
    int found = 0;
    EnterCriticalSection(&queue_lock);
    if (queue_head < queue_tail) {
        int idx = queue_head % QUEUE_SIZE;
        strcpy(out_path, job_queue[idx].path);
        queue_head++;
        found = 1;
    }
    LeaveCriticalSection(&queue_lock);
    return found;
}

// ==========================================
// THE WORKER (Win32 API Optimized)
// ==========================================
char TARGET_FILE[256];
size_t TARGET_LEN;

unsigned __stdcall worker_thread(void *arg) {
    char current_dir[MAX_PATH_LEN];
    char search_path[MAX_PATH_LEN];
    WIN32_FIND_DATAA find_data;
    HANDLE hFind;

    InterlockedIncrement(&active_workers);

    while (1) {
        if (pop_job(current_dir)) {
            // Prepare search pattern: current_dir\*
            snprintf(search_path, MAX_PATH_LEN, "%s\\*", current_dir);

            // UNHOLY OPTIMIZATION: FindExInfoBasic
            // Suppresses 8.3 short name generation. Faster on NTFS.
            hFind = FindFirstFileExA(
                search_path,
                FindExInfoBasic,
                &find_data,
                FindExSearchNameMatch,
                NULL,
                FIND_FIRST_EX_LARGE_FETCH // Hint to OS: we are reading a lot
            );

            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    // Skip . and ..
                    if (find_data.cFileName[0] == '.') {
                        if (find_data.cFileName[1] == '\0') continue;
                        if (find_data.cFileName[1] == '.' && find_data.cFileName[2] == '\0') continue;
                    }

                    // 1. AVX CHECK
                    if (avx2_strstr(find_data.cFileName, TARGET_FILE, TARGET_LEN)) {
                        // Use Windows Console API or printf (buffered usually okay here)
                        printf("FOUND: %s\\%s\n", current_dir, find_data.cFileName);
                    }

                    // 2. RECURSION
                    // Windows gives us the attribute for free! No stat() needed.
                    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        char new_path[MAX_PATH_LEN];
                        // Manual concatenation is faster than snprintf, but let's be safe first
                        if (snprintf(new_path, MAX_PATH_LEN, "%s\\%s", current_dir, find_data.cFileName) < MAX_PATH_LEN) {
                            push_job(new_path);
                        }
                    }

                } while (FindNextFileA(hFind, &find_data));
                FindClose(hFind);
            }
        } else {
            // Check exit condition
            if (finished_scanning && queue_head == queue_tail) break;
            SwitchToThread(); // Windows equivalent of sched_yield()
        }
    }
    InterlockedDecrement(&active_workers);
    return 0;
}

// ==========================================
// MAIN
// ==========================================
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: blade.exe <directory> <search_term>\n");
        printf("Example: blade.exe C:\\Windows system32\n");
        return 1;
    }

    strcpy(TARGET_FILE, argv[2]);
    TARGET_LEN = strlen(TARGET_FILE);
    InitializeCriticalSection(&queue_lock);

    printf("Starting Unholy Search for: '%s' in '%s'\n", TARGET_FILE, argv[1]);

    push_job(argv[1]);

    HANDLE threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        threads[i] = (HANDLE)_beginthreadex(NULL, 0, worker_thread, NULL, 0, NULL);
    }

    // Wait loop
    while (active_workers > 0 || queue_head != queue_tail) {
        Sleep(10); // Yield to workers
        if (queue_head == queue_tail) {
            Sleep(50); // Grace period
            if (queue_head == queue_tail) break;
        }
    }

    finished_scanning = 1;
    WaitForMultipleObjects(THREAD_COUNT, threads, TRUE, INFINITE);
    DeleteCriticalSection(&queue_lock);

    return 0;
}