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
#define ARENA_BLOCK_SIZE (32 * 1024 * 1024) // 32MB blocks for string arena
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
#define COL_DIM      RGB(100, 100, 100)

// ==========================================
// DATA STRUCTURES
// ==========================================
typedef struct {
    char *path;        // Points into Arena
    int is_dir;
    unsigned long long size;
    FILETIME write_time;
    
    // Drive specific info (only valid if is_drive=1)
    int is_drive;
    unsigned long long total_bytes;
    unsigned long long free_bytes;
    char fs_name[8];   // NTFS, FAT32
} Entry;

typedef enum { SORT_NAME=0, SORT_SIZE, SORT_DATE } SORT_MODE;

// Arena Allocator Structure
typedef struct ArenaBlock {
    char *data;
    size_t used;
    struct ArenaBlock *next;
} ArenaBlock;

// Global Storage
Entry *entries = NULL;
volatile long entry_count = 0;
long entry_capacity = 0;
CRITICAL_SECTION data_lock;

ArenaBlock *arena_head = NULL; // Head of string arena

// App State
volatile int running = 1;
volatile long active_workers = 0;
volatile long search_generation = 0;
SORT_MODE g_sort_mode = SORT_NAME;

char root_path[4096] = {0};
char search_buffer[256] = {0};
int is_wildcard = 0;

// Search Query Parsed
struct {
    char name[256];
    char ext[16];
    unsigned long long min_size;
    unsigned long long max_size;
} query;

// UI State
HWND hMainWnd;
int selected_index = 0;
int scroll_offset = 0;
int window_width = 1024;
int window_height = 768;
int max_visible_items = 0;
int is_truncated = 0;

// GDI Resources
HDC hdcBack = NULL;
HBITMAP hbmBack = NULL;
HBITMAP hbmOld = NULL;
HFONT hFont = NULL;
HFONT hFontSmall = NULL;

// ==========================================
// ARENA ALLOCATOR (Zero-Overhead Strings)
// ==========================================
void arena_init() {
    arena_head = NULL;
}

char* arena_alloc_str(const char *s) {
    size_t len = strlen(s) + 1;
    if (arena_head == NULL || (arena_head->used + len > ARENA_BLOCK_SIZE)) {
        ArenaBlock *b = (ArenaBlock*)malloc(sizeof(ArenaBlock));
        b->data = (char*)malloc(ARENA_BLOCK_SIZE);
        b->used = 0;
        b->next = arena_head;
        arena_head = b;
    }
    char *ret = arena_head->data + arena_head->used;
    memcpy(ret, s, len);
    arena_head->used += len;
    return ret;
}

void arena_free_all() {
    ArenaBlock *curr = arena_head;
    while (curr) {
        ArenaBlock *next = curr->next;
        free(curr->data);
        free(curr);
        curr = next;
    }
    arena_head = NULL;
}

// ==========================================
// UTILS & PARSING
// ==========================================
// FIX: Added missing stristr implementation
const char* stristr(const char* haystack, const char* needle) {
    if (!*needle) return haystack;
    for (; *haystack; ++haystack) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            const char *h, *n;
            for (h = haystack, n = needle; *h && *n; ++h, ++n) {
                if (tolower((unsigned char)*h) != tolower((unsigned char)*n)) break;
            }
            if (!*n) return haystack;
        }
    }
    return NULL;
}

unsigned long long parse_size_str(const char *s) {
    char *end;
    double val = strtod(s, &end);
    if (stristr(end, "g")) val *= 1024*1024*1024;
    else if (stristr(end, "m")) val *= 1024*1024;
    else if (stristr(end, "k")) val *= 1024;
    return (unsigned long long)val;
}

void parse_query() {
    memset(&query, 0, sizeof(query));
    char raw[256]; strcpy(raw, search_buffer);
    char *p = raw;
    char *tok = strtok(p, " ");
    while (tok) {
        if (strncmp(tok, "ext:", 4) == 0) {
            strncpy(query.ext, tok+4, 15);
            for(int i=0; query.ext[i]; i++) query.ext[i] = tolower(query.ext[i]);
        } else if (tok[0] == '>') {
            query.min_size = parse_size_str(tok+1);
        } else if (tok[0] == '<') {
            query.max_size = parse_size_str(tok+1);
        } else {
            if (query.name[0]) strcat(query.name, " ");
            strcat(query.name, tok);
        }
        tok = strtok(NULL, " ");
    }
    for(int i=0; query.name[i]; i++) query.name[i] = tolower(query.name[i]);
}

// ==========================================
// DATA MANAGEMENT
// ==========================================
void clear_data() {
    EnterCriticalSection(&data_lock);
    arena_free_all(); // Free all strings instantly
    entry_count = 0;
    selected_index = 0;
    scroll_offset = 0;
    is_truncated = 0;
    LeaveCriticalSection(&data_lock);
}

void add_entry_ex(const char *full, int dir, unsigned long long sz, const FILETIME *ft, 
                 int is_drive, unsigned long long tot, unsigned long long free_b, const char *fs) {
    if (entry_count >= MAX_RESULTS) { is_truncated = 1; return; }

    EnterCriticalSection(&data_lock);
    if (entry_count >= entry_capacity) {
        long new_cap = entry_capacity ? entry_capacity + (entry_capacity / 2) : INITIAL_CAPACITY;
        Entry *new_ptr = (Entry*)realloc(entries, new_cap * sizeof(Entry));
        if (new_ptr) { entries = new_ptr; entry_capacity = new_cap; }
        else { LeaveCriticalSection(&data_lock); return; }
    }

    Entry *e = &entries[entry_count++];
    e->path = arena_alloc_str(full);
    e->is_dir = dir;
    e->size = sz;
    if (ft) e->write_time = *ft;
    else memset(&e->write_time, 0, sizeof(FILETIME));

    e->is_drive = is_drive;
    if (is_drive) {
        e->total_bytes = tot;
        e->free_bytes = free_b;
        strncpy(e->fs_name, fs ? fs : "", 7);
    }
    LeaveCriticalSection(&data_lock);
}

// ==========================================
// AVX2 MATCHING
// ==========================================
int fast_glob_match(const char *text, const char *pattern) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1;
            while (*text) { if (fast_glob_match(text, pattern)) return 1; text++; }
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
// SORTING
// ==========================================
static const char* get_display_name(const char *path) {
    const char *slash = strrchr(path, '\\');
    if (slash && slash[1] != '\0') return slash + 1;
    return path;
}

int __cdecl entry_cmp(const void *pa, const void *pb) {
    const Entry *a = (const Entry*)pa;
    const Entry *b = (const Entry*)pb;

    int dirA = a->is_dir != 0;
    int dirB = b->is_dir != 0;
    if (dirA != dirB) return dirB - dirA; 

    switch (g_sort_mode) {
        case SORT_SIZE:
            if (a->size != b->size) return (b->size > a->size) ? 1 : -1;
            break;
        case SORT_DATE: {
            LONG c = CompareFileTime(&a->write_time, &b->write_time);
            if (c != 0) return -c; // Newest first
            break;
        }
    }
    return _stricmp(get_display_name(a->path), get_display_name(b->path));
}

void sort_entries() {
    EnterCriticalSection(&data_lock);
    if (entry_count > 1) qsort(entries, entry_count, sizeof(Entry), entry_cmp);
    LeaveCriticalSection(&data_lock);
    InvalidateRect(hMainWnd, NULL, FALSE);
}

// ==========================================
// SCANNING LOGIC
// ==========================================
void list_directory(const char *path) {
    clear_data();
    char spec[4096]; snprintf(spec, 4096, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileExA(spec, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') continue;
            char full[4096]; snprintf(full, 4096, "%s\\%s", path, fd.cFileName);
            unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            add_entry_ex(full, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), sz, &fd.ftLastWriteTime, 0, 0, 0, NULL);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    sort_entries();
}

void list_drives() {
    clear_data();
    DWORD drives = GetLogicalDrives();
    char root[] = "A:\\";
    for (int i = 0; i < 26; i++) {
        if (drives & (1 << i)) {
            root[0] = 'A' + i;
            ULARGE_INTEGER freeBytes, totalBytes, totalFree;
            char fs[16] = {0};
            GetDiskFreeSpaceExA(root, &freeBytes, &totalBytes, &totalFree);
            GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL, fs, 16);
            add_entry_ex(root, 1, 0, NULL, 1, totalBytes.QuadPart, totalFree.QuadPart, fs);
        }
    }
    InvalidateRect(hMainWnd, NULL, FALSE);
}

void scan_recursive(char *path, long gen) {
    if (gen != search_generation || !running) return;
    char spec[4096]; snprintf(spec, 4096, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileExA(spec, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) return;

    size_t name_len = strlen(query.name);

    do {
        if (gen != search_generation) break;
        if (fd.cFileName[0] == '.') continue;

        // 1. Name Filter
        int match = 0;
        if (name_len == 0) match = 1; // Empty search matches everything (if filters apply)
        else {
            if (is_wildcard) match = fast_glob_match(fd.cFileName, query.name);
            else match = avx2_strcasestr(fd.cFileName, query.name, name_len);
        }

        // 2. Extension Filter
        if (match && query.ext[0]) {
            const char *dot = strrchr(fd.cFileName, '.');
            if (!dot || _stricmp(dot, query.ext) != 0) match = 0;
        }

        // 3. Size Filter
        unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (match && query.min_size && sz < query.min_size) match = 0;
        if (match && query.max_size && sz > query.max_size) match = 0;

        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        char full[4096]; snprintf(full, 4096, "%s\\%s", path, fd.cFileName);

        if (match) add_entry_ex(full, is_dir, sz, &fd.ftLastWriteTime, 0, 0, 0, NULL);

        if (is_dir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) scan_recursive(full, gen);

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
            if (drives & (1 << i)) { d[0] = 'A' + i; scan_recursive(d, gen); }
        }
    } else scan_recursive(root_path, gen);
    InterlockedDecrement(&active_workers);
    InvalidateRect(hMainWnd, NULL, FALSE);
    return 0;
}

void refresh_state() {
    InterlockedIncrement(&search_generation);
    parse_query();
    
    // Heuristic: If no name/ext/size filter, browse mode
    int browse_mode = (strlen(search_buffer) == 0);

    if (browse_mode) {
        if (strlen(root_path) == 0) list_drives();
        else list_directory(root_path);
    } else {
        clear_data();
        is_wildcard = (strchr(query.name, '*') || strchr(query.name, '?'));
        for (int i = 0; i < THREAD_COUNT; i++) _beginthreadex(NULL, 0, hunter_thread, (void*)(intptr_t)search_generation, 0, NULL);
    }
    InvalidateRect(hMainWnd, NULL, FALSE);
}

// ==========================================
// NAVIGATION HELPERS
// ==========================================
void navigate_up() {
    if (strlen(root_path) == 0) return;
    char *last = strrchr(root_path, '\\');
    if (last) {
        if (last == strchr(root_path, '\\') && root_path[strlen(root_path)-1] == '\\') root_path[0] = 0;
        else { *last = 0; if (strlen(root_path) == 2 && root_path[1] == ':') strcat(root_path, "\\"); }
    } else root_path[0] = 0;
    search_buffer[0] = 0;
    refresh_state();
}

void open_path(const char *p) {
    ShellExecuteA(NULL, "open", p, NULL, NULL, SW_SHOWDEFAULT);
}

void open_in_explorer(const char *p) {
    char args[8192]; snprintf(args, 8192, "/select,\"%s\"", p);
    ShellExecuteA(NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
}

void copy_to_clipboard(const char *s) {
    if (!OpenClipboard(hMainWnd)) return;
    EmptyClipboard();
    size_t len = strlen(s) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    if (hMem) { memcpy(GlobalLock(hMem), s, len); GlobalUnlock(hMem); SetClipboardData(CF_TEXT, hMem); }
    CloseClipboard();
}

// ==========================================
// RENDERING
// ==========================================
void format_size(unsigned long long bytes, char *out) {
    if (bytes == 0) { strcpy(out, ""); return; }
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0; double d = (double)bytes;
    while (d > 1024 && i < 4) { d /= 1024; i++; }
    sprintf(out, "%.1f %s", d, u[i]);
}

void Render(HDC hdcDest) {
    if (!hdcBack) return;
    RECT rc = {0, 0, window_width, window_height};
    FillRect(hdcBack, &rc, CreateSolidBrush(COL_BG));
    RECT rcHead = {0, 0, window_width, HEADER_HEIGHT};
    FillRect(hdcBack, &rcHead, CreateSolidBrush(COL_HEADER));
    SetBkMode(hdcBack, TRANSPARENT);

    SelectObject(hdcBack, hFont);
    SetTextColor(hdcBack, COL_ACCENT);
    TextOutA(hdcBack, 10, 5, strlen(root_path) ? root_path : "This PC", strlen(root_path) ? strlen(root_path) : 7);

    SetTextColor(hdcBack, COL_TEXT);
    char prompt[300];
    if (strlen(search_buffer) > 0) snprintf(prompt, 300, "Query: %s  [Sort: F3/F4/F5]", search_buffer);
    else strcpy(prompt, "Type to hunt... (e.g. log ext:.txt >1mb)");
    SelectObject(hdcBack, hFontSmall);
    TextOutA(hdcBack, 10, 35, prompt, strlen(prompt));

    char stats[128];
    snprintf(stats, 128, "%ld items %s %s", entry_count, active_workers > 0 ? "[BUSY]" : "", is_truncated ? "[LIMIT]" : "");
    SIZE sz; GetTextExtentPoint32A(hdcBack, stats, strlen(stats), &sz);
    TextOutA(hdcBack, window_width - sz.cx - 10, 10, stats, strlen(stats));

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

        if (idx == selected_index) {
            RECT rcRow = {10, y, window_width - 10, y + ROW_HEIGHT};
            FillRect(hdcBack, &rcRow, CreateSolidBrush(COL_ACCENT));
            SetTextColor(hdcBack, COL_SEL_TEXT);
        } else SetTextColor(hdcBack, e->is_dir ? COL_DIR : COL_TEXT);

        const char *disp = get_display_name(e->path);
        TextOutA(hdcBack, 15, y, disp, strlen(disp));

        char meta[128] = {0};
        if (e->is_drive) {
            char szFree[32], szTot[32];
            format_size(e->free_bytes, szFree); format_size(e->total_bytes, szTot);
            snprintf(meta, 128, "%s free / %s (%s)", szFree, szTot, e->fs_name);
        } else if (!e->is_dir) {
            format_size(e->size, meta);
        }
        
        if (meta[0]) {
            SIZE ssz; GetTextExtentPoint32A(hdcBack, meta, strlen(meta), &ssz);
            TextOutA(hdcBack, window_width - ssz.cx - 15, y, meta, strlen(meta));
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
        case WM_TIMER: if (active_workers > 0) InvalidateRect(hwnd, NULL, FALSE); return 0;
        case WM_SIZE:
            window_width = LOWORD(lParam); window_height = HIWORD(lParam);
            { HDC h = GetDC(hwnd); if (hdcBack) DeleteDC(hdcBack); hdcBack = CreateCompatibleDC(h); hbmBack = CreateCompatibleBitmap(h, window_width, window_height); SelectObject(hdcBack, hbmBack); ReleaseDC(hwnd, h); }
            break;
        case WM_PAINT: { PAINTSTRUCT ps; HDC h = BeginPaint(hwnd, &ps); Render(h); EndPaint(hwnd, &ps); return 0; }
        case WM_MOUSEWHEEL: if ((short)HIWORD(wParam) > 0) scroll_offset -= 3; else scroll_offset += 3; if (scroll_offset < 0) scroll_offset = 0; InvalidateRect(hwnd, NULL, FALSE); return 0;
        case WM_CHAR:
            if (wParam == VK_ESCAPE) { if (strlen(search_buffer)>0) { search_buffer[0]=0; refresh_state(); } else PostQuitMessage(0); }
            else if (wParam == VK_BACK) { int l=strlen(search_buffer); if(l>0) search_buffer[l-1]=0; else navigate_up(); refresh_state(); }
            else if (wParam >= 32 && wParam < 127 && strlen(search_buffer) < 250) { int l=strlen(search_buffer); search_buffer[l]=(char)wParam; search_buffer[l+1]=0; refresh_state(); }
            return 0;
        case WM_KEYDOWN:
            switch(wParam) {
                case VK_UP: if (selected_index > 0) selected_index--; break;
                case VK_DOWN: selected_index++; break;
                case VK_PRIOR: selected_index -= max_visible_items; break;
                case VK_NEXT: selected_index += max_visible_items; break;
                case VK_F3: g_sort_mode = SORT_NAME; sort_entries(); break;
                case VK_F4: g_sort_mode = SORT_SIZE; sort_entries(); break;
                case VK_F5: g_sort_mode = SORT_DATE; sort_entries(); break;
                case VK_RETURN: {
                    if (GetKeyState(VK_CONTROL) & 0x8000) { EnterCriticalSection(&data_lock); if (entry_count > 0) open_in_explorer(entries[selected_index].path); LeaveCriticalSection(&data_lock); }
                    else { 
                        // Try to parse as path first
                        if (strchr(search_buffer, '\\') || (search_buffer[1] == ':')) {
                            DWORD a = GetFileAttributesA(search_buffer);
                            if (a != INVALID_FILE_ATTRIBUTES) {
                                if (a & FILE_ATTRIBUTE_DIRECTORY) { strcpy(root_path, search_buffer); search_buffer[0]=0; refresh_state(); }
                                else open_path(search_buffer);
                                break;
                            }
                        }
                        if (entry_count > 0) {
                            EnterCriticalSection(&data_lock);
                            Entry *e = &entries[selected_index];
                            if (e->is_dir) { strcpy(root_path, e->path); search_buffer[0]=0; refresh_state(); }
                            else open_path(e->path);
                            LeaveCriticalSection(&data_lock);
                        }
                    }
                    break;
                }
                case 'C': if ((GetKeyState(VK_CONTROL)&0x8000) && (GetKeyState(VK_SHIFT)&0x8000)) { EnterCriticalSection(&data_lock); if(entry_count>0) copy_to_clipboard(entries[selected_index].path); LeaveCriticalSection(&data_lock); } break;
                case 'L': if (GetKeyState(VK_CONTROL)&0x8000) copy_to_clipboard(root_path); break;
                case 'O': if (GetKeyState(VK_CONTROL)&0x8000 && root_path[0]) { char c[4096]; snprintf(c,4096,"/K cd /d \"%s\"", root_path); ShellExecuteA(NULL,"open","cmd.exe",c,root_path,SW_SHOWDEFAULT); } break;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_DESTROY: running=0; KillTimer(hwnd,1); clear_data(); free(entries); DeleteCriticalSection(&data_lock); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    entries = (Entry*)malloc(INITIAL_CAPACITY * sizeof(Entry));
    entry_capacity = INITIAL_CAPACITY;
    InitializeCriticalSection(&data_lock);
    if (__argc > 1) GetFullPathNameA(__argv[1], 4096, root_path, NULL);
    WNDCLASSA wc = {0}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "BladeClass"; RegisterClassA(&wc);
    hMainWnd = CreateWindowExA(0, "BladeClass", "Blade Explorer", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, NULL, NULL, hInst, NULL);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}