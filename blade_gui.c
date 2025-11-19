#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <immintrin.h>

// ==========================================
// CONFIGURATION
// ==========================================
#define MAX_PATH_LEN 4096
#define THREAD_COUNT 16
#define INITIAL_CAPACITY 65536
#define FONT_NAME "Segoe UI"
#define FONT_SIZE 18 // Height in pixels
#define BG_COLOR RGB(20, 20, 20)
#define FG_COLOR RGB(220, 220, 220)
#define ACCENT_COLOR RGB(0, 120, 215)
#define SELECTED_TEXT_COLOR RGB(255, 255, 255)

// ==========================================
// GLOBAL STATE
// ==========================================
typedef struct { char path[MAX_PATH_LEN]; } Result;

// Data
Result *results = NULL;
volatile long result_count = 0;
long result_capacity = 0;
CRITICAL_SECTION result_lock;

// Search State
volatile int running = 1;
volatile long active_workers = 0;
volatile long search_generation = 0; // For cancelling old searches
char root_directory[MAX_PATH_LEN];
char search_term[256] = {0};
char search_term_lower[256] = {0};
int is_wildcard = 0;

// UI State
HWND hMainWnd;
int selected_index = 0;
int scroll_offset = 0;
int window_width = 800;
int window_height = 600;
int max_visible_items = 0;
HFONT hFont;

// Double Buffering
HDC hdcBack = NULL;
HBITMAP hbmBack = NULL;
HBITMAP hbmOld = NULL;

// ==========================================
// PERFORMANCE SEARCH LOGIC (AVX2 + GLOB)
// ==========================================
// (Same engines as your CLI, optimized for generation checking)

int fast_glob_match(const char *text, const char *pattern) {
    // ... [Keep your existing glob_match code here] ...
    // Copied for completeness:
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
    // ... [Keep your existing AVX2 code here] ...
    if (needle_len == 0) return 1;
    char first_char = tolower(needle[0]);
    __m256i vec_first = _mm256_set1_epi8(first_char);
    __m256i vec_case_mask = _mm256_set1_epi8(0x20);
    for (size_t i = 0; haystack[i]; i += 32) {
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
// WORKER & DATA MANAGEMENT
// ==========================================
void reset_search() {
    EnterCriticalSection(&result_lock);
    result_count = 0;
    selected_index = 0;
    scroll_offset = 0;
    InterlockedIncrement(&search_generation);
    LeaveCriticalSection(&result_lock);
    InvalidateRect(hMainWnd, NULL, FALSE);
}

void add_result(const char *path) {
    EnterCriticalSection(&result_lock);
    if (result_count >= result_capacity) {
        long new_cap = result_capacity + (result_capacity / 2);
        Result *new_ptr = (Result*)realloc(results, new_cap * sizeof(Result));
        if (new_ptr) { results = new_ptr; result_capacity = new_cap; }
    }
    if (result_count < result_capacity) {
        strcpy(results[result_count].path, path);
        result_count++;
    }
    // Trigger repaint if this result might be visible (latency optimization)
    if (result_count < 50 || result_count % 100 == 0) {
        InvalidateRect(hMainWnd, NULL, FALSE);
    }
    LeaveCriticalSection(&result_lock);
}

// Recursive Directory Scan (Queue-less recursion for simpler state mgmt in GUI)
void scan_dir(char *path, long my_gen) {
    // ABORT IF GENERATION CHANGED (User typed something new)
    if (my_gen != search_generation || !running) return;

    char search_path[MAX_PATH_LEN];
    WIN32_FIND_DATAA find_data;
    sprintf(search_path, "%s\\*", path);

    HANDLE hFind = FindFirstFileExA(search_path, FindExInfoBasic, &find_data, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (my_gen != search_generation) break; // Hot path check

        if (find_data.cFileName[0] == '.') continue;

        int match = 0;
        if (is_wildcard) match = fast_glob_match(find_data.cFileName, search_term);
        else match = avx2_strcasestr(find_data.cFileName, search_term_lower, strlen(search_term));

        if (match) {
            char full_path[MAX_PATH_LEN];
            snprintf(full_path, MAX_PATH_LEN, "%s\\%s", path, find_data.cFileName);
            add_result(full_path);
        }

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub_path[MAX_PATH_LEN];
            snprintf(sub_path, MAX_PATH_LEN, "%s\\%s", path, find_data.cFileName);
            scan_dir(sub_path, my_gen);
        }
    } while (FindNextFileA(hFind, &find_data));
    FindClose(hFind);
}

unsigned __stdcall worker_thread(void *arg) {
    // Simple worker: just waits for a generation change, then scans
    // In a real GUI, we'd use a proper task queue, but for this example, 
    // we'll just have one master scanner for simplicity or launch scan on input.
    // To keep it simple for this "single file" demo, we will launch a thread per keystroke 
    // that cancels the previous one via the generation ID.
    
    long my_gen = (long)(intptr_t)arg;
    InterlockedIncrement(&active_workers);
    
    scan_dir(root_directory, my_gen);
    
    InterlockedDecrement(&active_workers);
    // Force final repaint to ensure "Scanning..." text disappears
    InvalidateRect(hMainWnd, NULL, FALSE);
    return 0;
}

void start_search_thread() {
    // Launch a detached thread
    _beginthreadex(NULL, 0, worker_thread, (void*)(intptr_t)search_generation, 0, NULL);
}

// ==========================================
// GRAPHICS (GDI DOUBLE BUFFERED)
// ==========================================
void InitDoubleBuffering(HDC hdc, int w, int h) {
    if (hdcBack) DeleteDC(hdcBack);
    if (hbmBack) DeleteObject(hbmBack);

    hdcBack = CreateCompatibleDC(hdc);
    hbmBack = CreateCompatibleBitmap(hdc, w, h);
    hbmOld = (HBITMAP)SelectObject(hdcBack, hbmBack);
}

void Render(HDC hdcDest) {
    if (!hdcBack) return;

    // 1. Clear Background
    RECT rc = {0, 0, window_width, window_height};
    HBRUSH bgBrush = CreateSolidBrush(BG_COLOR);
    FillRect(hdcBack, &rc, bgBrush);
    DeleteObject(bgBrush);

    // 2. Draw Header / Search Box
    int header_height = 40;
    RECT rcHeader = {0, 0, window_width, header_height};
    HBRUSH headerBrush = CreateSolidBrush(RGB(35, 35, 35));
    FillRect(hdcBack, &rcHeader, headerBrush);
    DeleteObject(headerBrush);

    SetBkMode(hdcBack, TRANSPARENT);
    SelectObject(hdcBack, hFont);
    SetTextColor(hdcBack, FG_COLOR);

    // Draw Search Term
    TextOutA(hdcBack, 10, 10, search_term, strlen(search_term));
    
    // Draw Cursor (Blinking logic omitted for raw speed/simplicity, just a static pipe)
    int search_len = strlen(search_term);
    SIZE sz;
    GetTextExtentPoint32A(hdcBack, search_term, search_len, &sz);
    TextOutA(hdcBack, 10 + sz.cx, 10, "_", 1);

    // Draw Status
    char status[128];
    snprintf(status, 128, "%ld hits %s", result_count, active_workers > 0 ? "(Scanning)" : "");
    SIZE szStatus;
    GetTextExtentPoint32A(hdcBack, status, strlen(status), &szStatus);
    SetTextColor(hdcBack, RGB(100, 100, 100));
    TextOutA(hdcBack, window_width - szStatus.cx - 10, 10, status, strlen(status));

    // 3. Draw List (Virtual Mode)
    int item_h = FONT_SIZE + 10;
    int start_y = header_height;
    max_visible_items = (window_height - header_height) / item_h;

    EnterCriticalSection(&result_lock);
    
    // Clamp scroll
    if (scroll_offset > result_count - max_visible_items) scroll_offset = result_count - max_visible_items;
    if (scroll_offset < 0) scroll_offset = 0;

    for (int i = 0; i < max_visible_items; i++) {
        int idx = scroll_offset + i;
        if (idx >= result_count) break;

        int y = start_y + (i * item_h);
        RECT rcItem = {0, y, window_width, y + item_h};

        // Selection Highlight
        if (idx == selected_index) {
            HBRUSH selBrush = CreateSolidBrush(ACCENT_COLOR);
            FillRect(hdcBack, &rcItem, selBrush);
            DeleteObject(selBrush);
            SetTextColor(hdcBack, SELECTED_TEXT_COLOR);
        } else {
            SetTextColor(hdcBack, FG_COLOR);
        }

        // Draw Text
        // Simple optimization: Truncate path if too long for screen? 
        // For now, just draw. GDI clips automatically.
        TextOutA(hdcBack, 10, y + 5, results[idx].path, strlen(results[idx].path));
    }
    LeaveCriticalSection(&result_lock);

    // 4. Blit to Screen
    BitBlt(hdcDest, 0, 0, window_width, window_height, hdcBack, 0, 0, SRCCOPY);
}

// ==========================================
// WINDOW PROCEDURE
// ==========================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE:
            hFont = CreateFontA(FONT_SIZE, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
            return 0;

        case WM_SIZE:
            window_width = LOWORD(lParam);
            window_height = HIWORD(lParam);
            {
                HDC hdc = GetDC(hwnd);
                InitDoubleBuffering(hdc, window_width, window_height);
                ReleaseDC(hwnd, hdc);
            }
            break;

        case WM_ERASEBKGND:
            return 1; // Prevent flickering, we handle paint

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            Render(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CHAR: {
            if (wParam == VK_ESCAPE) { PostQuitMessage(0); return 0; }
            if (wParam == VK_BACK) {
                int len = strlen(search_term);
                if (len > 0) search_term[len - 1] = 0;
            } else if (wParam >= 32 && wParam <= 126) {
                int len = strlen(search_term);
                if (len < 255) {
                    search_term[len] = (char)wParam;
                    search_term[len+1] = 0;
                }
            } else {
                return 0;
            }

            // Update Wildcard/Lower state
            is_wildcard = (strchr(search_term, '*') || strchr(search_term, '?'));
            for(int i=0; search_term[i]; i++) search_term_lower[i] = tolower(search_term[i]);
            search_term_lower[strlen(search_term)] = 0;

            reset_search();
            if (strlen(search_term) > 0) start_search_thread();
            
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            EnterCriticalSection(&result_lock);
            if (wParam == VK_DOWN) {
                if (selected_index < result_count - 1) selected_index++;
                if (selected_index >= scroll_offset + max_visible_items) scroll_offset++;
            }
            else if (wParam == VK_UP) {
                if (selected_index > 0) selected_index--;
                if (selected_index < scroll_offset) scroll_offset--;
            }
            else if (wParam == VK_PRIOR) { // Page Up
                selected_index -= max_visible_items;
                scroll_offset -= max_visible_items;
                if (selected_index < 0) selected_index = 0;
                if (scroll_offset < 0) scroll_offset = 0;
            }
            else if (wParam == VK_NEXT) { // Page Down
                selected_index += max_visible_items;
                scroll_offset += max_visible_items;
                if (selected_index >= result_count) selected_index = result_count - 1;
                if (scroll_offset > result_count - max_visible_items) scroll_offset = result_count - max_visible_items;
            }
            else if (wParam == VK_RETURN && result_count > 0) {
                // Open Explorer
                char param[MAX_PATH_LEN + 32];
                snprintf(param, sizeof(param), "/select,\"%s\"", results[selected_index].path);
                ShellExecuteA(NULL, "open", "explorer.exe", param, NULL, SW_SHOWDEFAULT);
            }
            LeaveCriticalSection(&result_lock);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ==========================================
// ENTRY POINT
// ==========================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    // Setup Data
    results = (Result*)malloc(INITIAL_CAPACITY * sizeof(Result));
    result_capacity = INITIAL_CAPACITY;
    InitializeCriticalSection(&result_lock);

    // Get initial directory (from args or current)
    if (__argc > 1) strcpy(root_directory, __argv[1]);
    else GetCurrentDirectoryA(MAX_PATH_LEN, root_directory);

    // Register Window Class
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "BladeWindowClass";
    // CS_HREDRAW | CS_VREDRAW causes full repaint on resize (good for layout)
    wc.style = CS_HREDRAW | CS_VREDRAW; 
    RegisterClassA(&wc);

    // Create Window
    // WS_EX_COMPOSITED enables automatic double-buffering at OS level (XP+)
    // But we did it manually above for maximum control. 
    hMainWnd = CreateWindowExA(0, "BladeWindowClass", "Blade Search",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInstance, NULL);

    // High Priority UI Thread
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // Message Loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    running = 0;
    DeleteCriticalSection(&result_lock);
    free(results);
    return 0;
}