// Force Windows Vista+ API
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <immintrin.h> // AVX2

// ==========================================
// CONFIGURATION
// ==========================================
#define MAX_RESULTS 200000
#define INITIAL_CAPACITY 16384
#define THREAD_COUNT 16
#define FONT_NAME "Segoe UI"
#define FONT_SIZE 20
#define ROW_HEIGHT 28
#define HEADER_HEIGHT 60

// Theme: "Void"
#define COL_BG       RGB(10, 10, 10)
#define COL_HEADER   RGB(20, 20, 20)
#define COL_TEXT     RGB(220, 220, 220)
#define COL_DIR      RGB(255, 215, 0)
#define COL_ACCENT   RGB(0, 120, 215)
#define COL_SEL_TEXT RGB(255, 255, 255)

// ==========================================
// DATA STRUCTURES
// ==========================================
typedef struct {
    char *path; // Dynamic string
    int is_dir;
    unsigned long long size;
} Entry;

// Global Storage
Entry *entries = NULL;
volatile long entry_count = 0;
long entry_capacity = 0;
CRITICAL_SECTION data_lock;

// App State
volatile int running = 1;
volatile long active_workers = 0;
volatile long search_generation = 0;

char root_path[4096] = {0}; // Current root
char search_buffer[256] = {0};
char search_lower[256] = {0};
size_t search_len = 0;
int is_wildcard = 0;

// UI State
HWND hMainWnd;
int selected_index = 0;
int scroll_offset = 0;
int window_width = 1024;
int window_height = 768;
int max_visible_items = 0;

// GDI Resources
HDC hdcBack = NULL;
HBITMAP hbmBack = NULL;
HBITMAP hbmOld = NULL;
HFONT hFont = NULL;
HFONT hFontSmall = NULL;

// ==========================================
// UTILS & MEMORY
// ==========================================
static char* dup_str(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = (char*)malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

void clear_data() {
    EnterCriticalSection(&data_lock);
    for (long i = 0; i < entry_count; ++i) {
        if (entries[i].path) free(entries[i].path);
    }
    entry_count = 0;
    selected_index = 0;
    scroll_offset = 0;
    LeaveCriticalSection(&data_lock);
}

void add_entry(const char *full, int dir, unsigned long long sz) {
    if (entry_count >= MAX_RESULTS) return;

    EnterCriticalSection(&data_lock);
    
    if (entry_count >= entry_capacity) {
        long new_cap = entry_capacity ? entry_capacity + (entry_capacity / 2) : INITIAL_CAPACITY;
        Entry *new_ptr = (Entry*)realloc(entries, new_cap * sizeof(Entry));
        if (!new_ptr) {
            LeaveCriticalSection(&data_lock);
            return;
        }
        entries = new_ptr;
        entry_capacity = new_cap;
    }

    if (entry_count < entry_capacity) {
        Entry *e = &entries[entry_count];
        e->path = dup_str(full);
        if (e->path) {
            e->is_dir = dir;
            e->size = sz;
            entry_count++;
        }
    }
    LeaveCriticalSection(&data_lock);
}

// Safe display name derivation (Fixes C:\ issue)
static const char* get_display_name(const char *path) {
    const char *slash = strrchr(path, '\\');
    if (slash && slash[1] != '\0') return slash + 1;
    return path;
}

// ==========================================
// EXPLORER BRIDGES
// ==========================================
void open_in_explorer(const Entry *e) {
    if (!e || !e->path) return;
    char full[4096];
    char *file_part = NULL;
    if (!GetFullPathNameA(e->path, sizeof(full), full, &file_part)) strcpy(full, e->path);

    char args[8192];
    snprintf(args, sizeof(args), "/select,\"%s\"", full);
    ShellExecuteA(NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
}

void copy_path_to_clipboard(const char *path) {
    if (!path || !*path) return;
    if (!OpenClipboard(hMainWnd)) return;
    EmptyClipboard();
    size_t len = strlen(path) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (hMem) {
        char *dst = (char*)GlobalLock(hMem);
        memcpy(dst, path, len);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
}

// ==========================================
// SORTING
// ==========================================
int __cdecl entry_cmp(const void *pa, const void *pb) {
    const Entry *a = (const Entry*)pa;
    const Entry *b = (const Entry*)pb;

    int dirA = a->is_dir != 0;
    int dirB = b->is_dir != 0;
    if (dirA != dirB) return dirB - dirA; // directories first

    const char *nameA = get_display_name(a->path);
    const char *nameB = get_display_name(b->path);
    return _stricmp(nameA, nameB);
}

// ==========================================
// AVX2 SEARCH ENGINE
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
// FILE SYSTEM LOGIC
// ==========================================
void list_directory(const char *path) {
    clear_data();
    char search_spec[4096];
    snprintf(search_spec, 4096, "%s\\*", path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileExA(search_spec, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') continue;
            char full[4096];
            snprintf(full, 4096, "%s\\%s", path, fd.cFileName);
            unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            add_entry(full, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), sz);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    // Optimized QSort
    EnterCriticalSection(&data_lock);
    if (entry_count > 1) qsort(entries, entry_count, sizeof(Entry), entry_cmp);
    LeaveCriticalSection(&data_lock);
    InvalidateRect(hMainWnd, NULL, FALSE);
}

void list_drives() {
    clear_data();
    DWORD drives = GetLogicalDrives();
    char d[] = "A:\\";
    for (int i = 0; i < 26; i++) {
        if (drives & (1 << i)) {
            d[0] = 'A' + i;
            add_entry(d, 1, 0);
        }
    }
    InvalidateRect(hMainWnd, NULL, FALSE);
}

void scan_recursive(char *path, long gen) {
    if (gen != search_generation || !running) return;

    char spec[4096];
    snprintf(spec, 4096, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileExA(spec, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (gen != search_generation) break;
        if (fd.cFileName[0] == '.') continue;

        int match = 0;
        if (is_wildcard) match = fast_glob_match(fd.cFileName, search_buffer);
        else match = avx2_strcasestr(fd.cFileName, search_lower, search_len);

        char full[4096];
        snprintf(full, 4096, "%s\\%s", path, fd.cFileName);
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

        if (match) {
            unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            add_entry(full, is_dir, sz);
        }

        if (is_dir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            scan_recursive(full, gen);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}

unsigned __stdcall hunter_thread(void *arg) {
    long gen = (long)(intptr_t)arg;
    InterlockedIncrement(&active_workers);
    if (strlen(root_path) == 0) {
        DWORD drives = GetLogicalDrives();
        char d[] = "A:\\";
        for (int i = 0; i < 26; i++) {
            if (gen != search_generation) break;
            if (drives & (1 << i)) {
                d[0] = 'A' + i;
                scan_recursive(d, gen);
            }
        }
    } else {
        scan_recursive(root_path, gen);
    }
    InterlockedDecrement(&active_workers);
    InvalidateRect(hMainWnd, NULL, FALSE);
    return 0;
}

// ==========================================
// NAVIGATION
// ==========================================
void refresh_state() {
    InterlockedIncrement(&search_generation);
    if (strlen(search_buffer) == 0) {
        if (strlen(root_path) == 0) list_drives();
        else list_directory(root_path);
    } else {
        clear_data();
        for (int i = 0; i < THREAD_COUNT; i++) {
            _beginthreadex(NULL, 0, hunter_thread, (void*)(intptr_t)search_generation, 0, NULL);
        }
    }
    InvalidateRect(hMainWnd, NULL, FALSE);
}

void navigate_up() {
    if (strlen(root_path) == 0) return;
    char *last = strrchr(root_path, '\\');
    if (last) {
        if (last == strchr(root_path, '\\') && root_path[strlen(root_path)-1] == '\\') root_path[0] = 0;
        else {
            *last = 0;
            if (strlen(root_path) == 2 && root_path[1] == ':') strcat(root_path, "\\");
        }
    } else root_path[0] = 0;
    
    search_buffer[0] = 0;
    refresh_state();
}

void navigate_down() {
    EnterCriticalSection(&data_lock);
    if (entry_count > 0 && selected_index < entry_count) {
        Entry *e = &entries[selected_index];
        if (e->is_dir) {
            strcpy(root_path, e->path);
            search_buffer[0] = 0;
            LeaveCriticalSection(&data_lock);
            refresh_state();
        } else {
            ShellExecuteA(NULL, "open", e->path, NULL, NULL, SW_SHOWDEFAULT);
            LeaveCriticalSection(&data_lock);
        }
    } else LeaveCriticalSection(&data_lock);
}

// ==========================================
// RENDERING
// ==========================================
void InitDoubleBuffer(HDC hdc, int w, int h) {
    if (hdcBack) DeleteDC(hdcBack);
    if (hbmBack) DeleteObject(hbmBack);
    hdcBack = CreateCompatibleDC(hdc);
    hbmBack = CreateCompatibleBitmap(hdc, w, h);
    hbmOld = (HBITMAP)SelectObject(hdcBack, hbmBack);
}

void format_size(unsigned long long bytes, char *out) {
    if (bytes == 0) { strcpy(out, ""); return; }
    const char *u[] = {"B", "KB", "MB", "GB"};
    int i = 0;
    double d = (double)bytes;
    while (d > 1024 && i < 3) { d /= 1024; i++; }
    sprintf(out, "%.1f %s", d, u[i]);
}

void Render(HDC hdcDest) {
    if (!hdcBack) return;

    RECT rc = {0, 0, window_width, window_height};
    HBRUSH hBg = CreateSolidBrush(COL_BG);
    FillRect(hdcBack, &rc, hBg);
    DeleteObject(hBg);

    RECT rcHead = {0, 0, window_width, HEADER_HEIGHT};
    HBRUSH hHeader = CreateSolidBrush(COL_HEADER);
    FillRect(hdcBack, &rcHead, hHeader);
    DeleteObject(hHeader);

    SetBkMode(hdcBack, TRANSPARENT);

    // Header Text
    SelectObject(hdcBack, hFont);
    SetTextColor(hdcBack, COL_ACCENT);
    TextOutA(hdcBack, 10, 5, strlen(root_path) ? root_path : "This PC", strlen(root_path) ? strlen(root_path) : 7);

    // Search Prompt
    SetTextColor(hdcBack, COL_TEXT);
    char prompt[300];
    if (strlen(search_buffer) > 0) snprintf(prompt, 300, "Finding: %s_", search_buffer);
    else strcpy(prompt, "Type to hunt files...");
    SelectObject(hdcBack, hFontSmall);
    TextOutA(hdcBack, 10, 35, prompt, strlen(prompt));

    // Stats
    char stats[64];
    snprintf(stats, 64, "%ld items %s", entry_count, active_workers > 0 ? "[BUSY]" : "");
    SIZE sz; GetTextExtentPoint32A(hdcBack, stats, strlen(stats), &sz);
    TextOutA(hdcBack, window_width - sz.cx - 10, 10, stats, strlen(stats));

    // List
    EnterCriticalSection(&data_lock);
    int start_y = HEADER_HEIGHT + 5;
    max_visible_items = (window_height - start_y) / ROW_HEIGHT;

    if (selected_index >= entry_count) selected_index = entry_count - 1;
    if (selected_index < 0) selected_index = 0;
    if (selected_index < scroll_offset) scroll_offset = selected_index;
    if (selected_index >= scroll_offset + max_visible_items) scroll_offset = selected_index - max_visible_items + 1;

    SelectObject(hdcBack, hFont);
    for (int i = 0; i < max_visible_items; i++) {
        int idx = scroll_offset + i;
        if (idx >= entry_count) break;
        
        int y = start_y + (i * ROW_HEIGHT);
        Entry *e = &entries[idx];
        
        // FIX: Use safe display name helper
        const char *disp = get_display_name(e->path);

        // Highlight
        if (idx == selected_index) {
            RECT rcRow = {10, y, window_width - 10, y + ROW_HEIGHT};
            HBRUSH hSel = CreateSolidBrush(COL_ACCENT);
            FillRect(hdcBack, &rcRow, hSel);
            DeleteObject(hSel);
            SetTextColor(hdcBack, COL_SEL_TEXT);
        } else {
            SetTextColor(hdcBack, e->is_dir ? COL_DIR : COL_TEXT);
        }

        TextOutA(hdcBack, 15, y, disp, strlen(disp));

        if (!e->is_dir) {
            char sz[32]; format_size(e->size, sz);
            SIZE ssz; GetTextExtentPoint32A(hdcBack, sz, strlen(sz), &ssz);
            TextOutA(hdcBack, window_width - ssz.cx - 15, y, sz, strlen(sz));
        }
    }
    LeaveCriticalSection(&data_lock);

    BitBlt(hdcDest, 0, 0, window_width, window_height, hdcBack, 0, 0, SRCCOPY);
}

// ==========================================
// WINDOW PROC
// ==========================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE:
            hFont = CreateFontA(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
            hFontSmall = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
            refresh_state();
            SetTimer(hwnd, 1, 100, NULL);
            return 0;

        case WM_TIMER:
            if (active_workers > 0) InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_SIZE:
            window_width = LOWORD(lParam);
            window_height = HIWORD(lParam);
            {
                HDC hdc = GetDC(hwnd);
                InitDoubleBuffer(hdc, window_width, window_height);
                ReleaseDC(hwnd, hdc);
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            Render(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEWHEEL:
            if ((short)HIWORD(wParam) > 0) scroll_offset -= 3;
            else scroll_offset += 3;
            if (scroll_offset < 0) scroll_offset = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_CHAR:
            if (wParam == VK_ESCAPE) {
                if (strlen(search_buffer) > 0) { search_buffer[0] = 0; refresh_state(); }
                else PostQuitMessage(0);
            } else if (wParam == VK_BACK) {
                size_t len = strlen(search_buffer);
                if (len > 0) {
                    search_buffer[len-1] = 0;
                    search_len = len - 1;
                    for(int i=0; i<256; i++) search_lower[i] = tolower(search_buffer[i]);
                    refresh_state();
                } else navigate_up();
            } else if (wParam >= 32 && wParam < 127) {
                size_t len = strlen(search_buffer);
                if (len < 250) {
                    search_buffer[len] = (char)wParam;
                    search_buffer[len+1] = 0;
                    search_len = len + 1;
                    is_wildcard = (strchr(search_buffer, '*') || strchr(search_buffer, '?'));
                    for(int i=0; i<256; i++) search_lower[i] = tolower(search_buffer[i]);
                    refresh_state();
                }
            }
            return 0;

        case WM_KEYDOWN:
            switch(wParam) {
                case VK_UP: if(selected_index > 0) selected_index--; break;
                case VK_DOWN: selected_index++; break;
                case VK_PRIOR: selected_index -= max_visible_items; break;
                case VK_NEXT: selected_index += max_visible_items; break;
                
                case VK_RETURN: {
                    SHORT ctrl = GetKeyState(VK_CONTROL);
                    // Ctrl+Enter: Open /select
                    if (ctrl & 0x8000) {
                         EnterCriticalSection(&data_lock);
                         if (entry_count > 0 && selected_index >= 0 && selected_index < entry_count) 
                            open_in_explorer(&entries[selected_index]);
                         LeaveCriticalSection(&data_lock);
                         break;
                    }
                    // Path input navigation
                    if (search_buffer[0]) {
                         char cand[4096];
                         if ((search_buffer[1] == ':' && (search_buffer[2] == '\\' || search_buffer[2] == '/')) || search_buffer[0] == '\\') {
                             strcpy(cand, search_buffer);
                         } else if (root_path[0]) {
                             snprintf(cand, 4096, "%s\\%s", root_path, search_buffer);
                         } else strcpy(cand, search_buffer);

                         DWORD attrs = GetFileAttributesA(cand);
                         if (attrs != INVALID_FILE_ATTRIBUTES) {
                             if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                                 strcpy(root_path, cand);
                                 search_buffer[0] = 0;
                                 refresh_state();
                             } else ShellExecuteA(NULL, "open", cand, NULL, NULL, SW_SHOWDEFAULT);
                             break;
                         }
                    }
                    navigate_down();
                    break;
                }

                case 'C': {
                    SHORT ctrl = GetKeyState(VK_CONTROL);
                    SHORT shift = GetKeyState(VK_SHIFT);
                    if ((ctrl & 0x8000) && (shift & 0x8000)) { // Ctrl+Shift+C
                        EnterCriticalSection(&data_lock);
                        if (entry_count > 0 && selected_index < entry_count)
                            copy_path_to_clipboard(entries[selected_index].path);
                        LeaveCriticalSection(&data_lock);
                    }
                    break;
                }
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_DESTROY:
            running = 0;
            KillTimer(hwnd, 1);
            if (hdcBack) {
                SelectObject(hdcBack, hbmOld);
                DeleteObject(hbmBack);
                DeleteDC(hdcBack);
            }
            if (hFont) DeleteObject(hFont);
            if (hFontSmall) DeleteObject(hFontSmall);
            clear_data();
            free(entries);
            DeleteCriticalSection(&data_lock);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    entries = (Entry*)malloc(INITIAL_CAPACITY * sizeof(Entry));
    entry_capacity = INITIAL_CAPACITY;
    InitializeCriticalSection(&data_lock);

    if (__argc > 1) GetFullPathNameA(__argv[1], 4096, root_path, NULL);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "BladeClass";
    RegisterClassA(&wc);

    hMainWnd = CreateWindowExA(0, "BladeClass", "Blade", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, NULL, NULL, hInst, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}