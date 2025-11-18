#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <immintrin.h> // AVX2

// ==========================================
// CONFIGURATION
// ==========================================
#define MAX_PATH_LEN 4096
#define THREAD_COUNT 16   // Bumped for modern CPUs
#define QUEUE_SIZE 16384 // Deeper queue for massive C:\ recursive scans

// ==========================================
// FAST GLOB MATCHER (Scalar)
// ==========================================
// Matches "start*.py", "*.txt", "invoice_???.pdf"
// Case insensitive for Windows behavior.
int fast_glob_match(const char *text, const char *pattern) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++; // Skip multiple asterisks
            if (!*pattern) return 1; // Trailing * matches everything
            
            // Try to match the rest of the pattern against every suffix of text
            while (*text) {
                if (fast_glob_match(text, pattern)) return 1;
                text++;
            }
            return 0;
        }
        
        // Case insensitive character comparison
        char t = tolower((unsigned char)*text);
        char p = tolower((unsigned char)*pattern);
        
        if (t != p && *pattern != '?') return 0;
        if (!*text) return 0; // Pattern expects char but text ended
        
        text++; 
        pattern++;
    }
    return !*text; // Match if both ended
}

// ==========================================
// AVX2 CASE-INSENSITIVE SUBSTRING ENGINE
// ==========================================
// Used when no wildcards are present.
// Uses AVX2 to scan, but handles casing by normalizing A-Z to a-z via OR 0x20
int avx2_strcasestr(const char *haystack, const char *needle, size_t needle_len) {
    if (needle_len == 0) return 1;
    
    // Broadcast first char of needle (lowercased)
    char first_char = tolower(needle[0]);
    __m256i vec_first = _mm256_set1_epi8(first_char);
    
    // Vector for case conversion (OR with 0x20 converts A->a, B->b... leaves others mostly capable)
    // This is a "dirty" lowercase that works for ASCII.
    __m256i vec_case_mask = _mm256_set1_epi8(0x20);

    size_t i = 0;
    for (; haystack[i]; i += 32) {
        // Load 32 bytes
        __m256i block = _mm256_loadu_si256((const __m256i*)(haystack + i));
        
        // "Dirty" lowercase the block: (block | 0x20)
        // NOTE: This might corrupt numbers/symbols slightly but for 'checking equality' against a known lowercase char it works well enough for speed.
        // For strict correctness we'd check ranges, but we accept the speed tradeoff here for the candidate check.
        __m256i block_lower = _mm256_or_si256(block, vec_case_mask);
        
        // Compare against first char
        __m256i eq = _mm256_cmpeq_epi8(vec_first, block_lower);
        unsigned int mask = _mm256_movemask_epi8(eq);
        
        while (mask) {
            int bit_pos = __builtin_ctz(mask);
            // Scalar verification (strictly correct strncasecmp)
            if (strncasecmp(haystack + i + bit_pos, needle, needle_len) == 0) return 1;
            mask &= ~(1 << bit_pos);
        }
    }
    return 0;
}

// MinGW doesn't always have strncasecmp, map it to Windows specific
int strncasecmp(const char *s1, const char *s2, size_t n) {
    return _strnicmp(s1, s2, n);
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
// WORKER THREAD
// ==========================================
char TARGET_RAW[256];
char TARGET_LOWER[256];
size_t TARGET_LEN;
int IS_WILDCARD = 0;

unsigned __stdcall worker_thread(void *arg) {
    char current_dir[MAX_PATH_LEN];
    char search_path[MAX_PATH_LEN];
    WIN32_FIND_DATAA find_data;
    HANDLE hFind;

    InterlockedIncrement(&active_workers);

    while (1) {
        if (pop_job(current_dir)) {
            snprintf(search_path, MAX_PATH_LEN, "%s\\*", current_dir);

            hFind = FindFirstFileExA(
                search_path,
                FindExInfoBasic,
                &find_data,
                FindExSearchNameMatch,
                NULL,
                FIND_FIRST_EX_LARGE_FETCH 
            );

            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    // Skip . and ..
                    if (find_data.cFileName[0] == '.') {
                        if (find_data.cFileName[1] == '\0') continue;
                        if (find_data.cFileName[1] == '.' && find_data.cFileName[2] == '\0') continue;
                    }

                    // === MATCH CHECK ===
                    int match = 0;
                    if (IS_WILDCARD) {
                        if (fast_glob_match(find_data.cFileName, TARGET_RAW)) match = 1;
                    } else {
                        // Use AVX2 engine
                        if (avx2_strcasestr(find_data.cFileName, TARGET_LOWER, TARGET_LEN)) match = 1;
                    }

                    if (match) {
                        // Using printf allows buffering. For ultimate console speed, accumulate in buffer, but printf is fine.
                        printf("%s\\%s\n", current_dir, find_data.cFileName);
                    }

                    // === RECURSION ===
                    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        char new_path[MAX_PATH_LEN];
                        if (snprintf(new_path, MAX_PATH_LEN, "%s\\%s", current_dir, find_data.cFileName) < MAX_PATH_LEN) {
                            push_job(new_path);
                        }
                    }

                } while (FindNextFileA(hFind, &find_data));
                FindClose(hFind);
            }
        } else {
            if (finished_scanning && queue_head == queue_tail) break;
            SwitchToThread();
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
        return 1;
    }

    // Prepare Search Term
    strcpy(TARGET_RAW, argv[2]);
    TARGET_LEN = strlen(TARGET_RAW);
    
    // Check for wildcards
    if (strchr(TARGET_RAW, '*') || strchr(TARGET_RAW, '?')) {
        IS_WILDCARD = 1;
    }
    
    // Prepare lowercase version for AVX
    for(int i=0; i<TARGET_LEN; i++) TARGET_LOWER[i] = tolower(TARGET_RAW[i]);
    TARGET_LOWER[TARGET_LEN] = 0;

    InitializeCriticalSection(&queue_lock);

    // Start
    push_job(argv[1]);

    HANDLE threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        threads[i] = (HANDLE)_beginthreadex(NULL, 0, worker_thread, NULL, 0, NULL);
    }

    // Wait for completion
    while (active_workers > 0 || queue_head != queue_tail) {
        Sleep(10); 
        if (queue_head == queue_tail) {
            Sleep(100); // Grace period
            if (queue_head == queue_tail) break;
        }
    }

    finished_scanning = 1;
    WaitForMultipleObjects(THREAD_COUNT, threads, TRUE, INFINITE);
    DeleteCriticalSection(&queue_lock);

    return 0;
}