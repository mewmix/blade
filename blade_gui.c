// Compile with: cl blade.c user32.lib gdi32.lib shell32.lib ole32.lib /O2 /arch:AVX2
// Or MinGW: gcc blade.c -o blade.exe -lgdi32 -luser32 -lshell32 -lole32 -mavx2 -O3

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // Force Vista+ for SHGetKnownFolderPath
#endif

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h> // For SHGetKnownFolderPath
#include <knownfolders.h> // For FOLDERID_* constants (Vista+)
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <immintrin.h> // AVX2

// Some SDKs gate this behind _WIN32_WINNT >= 0x0601 (Win7)
// Provide the constant if missing so FindFirstFileExA can use large fetch.
#ifndef FIND_FIRST_EX_LARGE_FETCH
#define FIND_FIRST_EX_LARGE_FETCH 2
#endif

// ==========================================
// CONFIGURATION
// ==========================================
#define MAX_RESULTS 200000
#define INITIAL_CAPACITY 16384
#define THREAD_COUNT 16
#define ARENA_BLOCK_SIZE (32 * 1024 * 1024)
#define FONT_NAME "Segoe UI"
#define FONT_SIZE 20
#define ROW_HEIGHT 28
#define HEADER_HEIGHT 70
#define MAX_FAVORITES 20

// Context menu command IDs
#define CMD_OPEN            1001
#define CMD_OPEN_EXPLORER   1002
#define CMD_NEW_FOLDER      1003
#define CMD_COPY_ENTRY      1004
#define CMD_PASTE_ENTRY     1005
#define CMD_DELETE_ENTRY    1006
#define CMD_RENAME_ENTRY    1007
#define CMD_REMOVE_FAV      1008

// Theme: "Void"
#define COL_BG       RGB(10, 10, 10)
#define COL_HEADER   RGB(20, 20, 20)
#define COL_TEXT     RGB(220, 220, 220)
#define COL_DIR      RGB(255, 215, 0)
#define COL_ACCENT   RGB(0, 120, 215)
#define COL_SEL_TEXT RGB(255, 255, 255)
#define COL_DIM      RGB(100, 100, 100)
#define COL_HOVER    RGB(30, 30, 30)
#define COL_HELP_BG  RGB(30, 30, 30)
#define COL_RECYCLED RGB(150, 80, 80) // Reddish dim for deleted items

// ==========================================
// DATA STRUCTURES
// ==========================================
typedef struct {
    char *path;        // Points into Arena
    int is_dir;
    unsigned long long size;
    FILETIME write_time;
    int is_drive;
    int is_recycled;   // <--- NEW: Flag for recycle bin
    int is_favorite;   // <--- NEW: Flag for favorites
    unsigned long long total_bytes;
    unsigned long long free_bytes;
    char fs_name[8];
} Entry;

typedef enum { SORT_NAME=0, SORT_SIZE, SORT_DATE } SORT_MODE;
typedef enum { CTRL_O_WT=0, CTRL_O_CMD, CTRL_O_EXPLORER } CTRL_O_MODE;

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

ArenaBlock *arena_head = NULL; 

// Favorites Storage
char favorites[MAX_FAVORITES][MAX_PATH];
int fav_count = 0;

// App State
volatile int running = 1;
volatile long active_workers = 0;
volatile long search_generation = 0;
SORT_MODE g_sort_mode = SORT_NAME;
CTRL_O_MODE g_ctrl_o_mode = CTRL_O_WT;

char root_path[4096] = {0};
char search_buffer[256] = {0};
char g_ini_path[MAX_PATH] = {0};
int is_wildcard = 0;
int show_help = 0;

// Copy/paste state
char g_copy_source[4096] = {0};
int  g_copy_is_dir = 0;

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
int hover_index = -1;
int scroll_offset = 0;
int window_width = 1024;
int window_height = 768;
int max_visible_items = 0;
int is_truncated = 0;
int list_start_y = 0;

// GDI Resources
HDC hdcBack = NULL;
HBITMAP hbmBack = NULL;
HBITMAP hbmOld = NULL;
HFONT hFont = NULL;
HFONT hFontSmall = NULL;
HFONT hFontMono = NULL;
HFONT hFontStrike = NULL; // <--- NEW: Strikethrough font

// ==========================================
// UTILS & PARSING
// ==========================================
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
// ARENA ALLOCATOR
// ==========================================
void arena_init() { arena_head = NULL; }

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
// FAVORITES / RECENT (VISTA+ API)
// ==========================================
void get_favorites_path(char *out_path) {
    PWSTR localAppData = NULL;
    // Force Vista+ API: SHGetKnownFolderPath
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &localAppData))) {
        wcstombs(out_path, localAppData, MAX_PATH);
        CoTaskMemFree(localAppData);
        strcat(out_path, "\\BladeExplorer");
        CreateDirectoryA(out_path, NULL); // Ensure dir exists
        strcat(out_path, "\\recent.dat");
    } else {
        // Fallback
        strcpy(out_path, "recent.dat");
    }
}

void save_favorites() {
    char path[MAX_PATH];
    get_favorites_path(path);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(&fav_count, sizeof(int), 1, f);
        for (int i=0; i<fav_count; i++) {
            fwrite(favorites[i], 1, MAX_PATH, f);
        }
        fclose(f);
    }
}

void load_favorites() {
    char path[MAX_PATH];
    get_favorites_path(path);
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fread(&fav_count, sizeof(int), 1, f) == 1) {
            if (fav_count > MAX_FAVORITES) fav_count = MAX_FAVORITES;
            for (int i=0; i<fav_count; i++) {
                fread(favorites[i], 1, MAX_PATH, f);
            }
        }
        fclose(f);
    }
}

void add_favorite(const char *path) {
    // Check duplicates
    for (int i = 0; i < fav_count; i++) {
        if (_stricmp(favorites[i], path) == 0) {
            // Move to top
            char temp[MAX_PATH];
            strcpy(temp, favorites[i]);
            for (int k = i; k > 0; k--) strcpy(favorites[k], favorites[k-1]);
            strcpy(favorites[0], temp);
            save_favorites();
            return;
        }
    }
    // Shift down
    if (fav_count < MAX_FAVORITES) fav_count++;
    for (int i = fav_count - 1; i > 0; i--) {
        strcpy(favorites[i], favorites[i-1]);
    }
    strncpy(favorites[0], path, MAX_PATH);
    save_favorites();
}

// ==========================================
// SETTINGS LOADING
// ==========================================
void load_settings() {
    GetModuleFileNameA(NULL, g_ini_path, MAX_PATH);
    char *last_slash = strrchr(g_ini_path, '\\');
    if (last_slash) *(last_slash + 1) = 0;
    strcat(g_ini_path, "blade.ini");

    char buf[32] = {0};
    GetPrivateProfileStringA("General", "CtrlO", "wt", buf, 32, g_ini_path);
    
    if (_stricmp(buf, "cmd") == 0) g_ctrl_o_mode = CTRL_O_CMD;
    else if (_stricmp(buf, "explorer") == 0) g_ctrl_o_mode = CTRL_O_EXPLORER;
    else g_ctrl_o_mode = CTRL_O_WT;
}

// ==========================================
// DATA MANAGEMENT
// ==========================================
void clear_data() {
    EnterCriticalSection(&data_lock);
    arena_free_all(); 
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
    e->is_recycled = 0;
    e->is_favorite = 0;

    // Detect Recycle Bin
    if (stristr(full, "$Recycle.Bin") || stristr(full, "\\RECYCLER\\")) {
        e->is_recycled = 1;
    }

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

    // 1. Favorites/Drives at top usually, but inside folder view:
    // ALWAYS sort directories before files
    int dirA = a->is_dir != 0;
    int dirB = b->is_dir != 0;
    if (dirA != dirB) return dirB - dirA; // 1 = dir, 0 = file. Returns positive if a is dir and b is file.

    // 2. Apply requested sort mode
    switch (g_sort_mode) {
        case SORT_SIZE:
            if (a->size != b->size) return (b->size > a->size) ? 1 : -1;
            break;
        case SORT_DATE: {
            LONG c = CompareFileTime(&a->write_time, &b->write_time);
            if (c != 0) return -c;
            break;
        }
    }
    // 3. Fallback to Name
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

void list_drives_and_favs() {
    clear_data();
    
    // 1. Add Favorites first
    for(int i = 0; i < fav_count; i++) {
        EnterCriticalSection(&data_lock);
        if (entry_count < MAX_RESULTS) {
            Entry *e = &entries[entry_count];
            // Simple fake attributes for favorites
            e->path = arena_alloc_str(favorites[i]);
            DWORD attr = GetFileAttributesA(favorites[i]);
            e->is_dir = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
            e->is_drive = 0;
            e->is_favorite = 1;
            e->size = 0;
            memset(&e->write_time, 0, sizeof(FILETIME));
            entry_count++;
        }
        LeaveCriticalSection(&data_lock);
    }

    // 2. Add Drives
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
    // No sort here, we want Favorites at top, then drives A-Z
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

        int match = 0;
        if (name_len == 0) match = 1; 
        else {
            if (is_wildcard) match = fast_glob_match(fd.cFileName, query.name);
            else match = avx2_strcasestr(fd.cFileName, query.name, name_len);
        }

        if (match && query.ext[0]) {
            const char *dot = strrchr(fd.cFileName, '.');
            if (!dot || _stricmp(dot, query.ext) != 0) match = 0;
        }

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

    char target_path[4096] = {0};
    int is_absolute = (search_buffer[1] == ':' || (search_buffer[0] == '\\' && search_buffer[1] == '\\'));

    if (is_absolute) {
        lstrcpynA(target_path, search_buffer, 4096);
    } 
    else if (strlen(root_path) > 0) {
        size_t root_len = strlen(root_path);
        if (root_path[root_len - 1] == '\\')
            snprintf(target_path, 4096, "%s%s", root_path, search_buffer);
        else
            snprintf(target_path, 4096, "%s\\%s", root_path, search_buffer);
    }

    DWORD attr = (target_path[0]) ? GetFileAttributesA(target_path) : INVALID_FILE_ATTRIBUTES;
    int is_valid_dir = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));

    if (strlen(search_buffer) == 0) {
        if (strlen(root_path) == 0) list_drives_and_favs();
        else list_directory(root_path);
    }
    else if (is_valid_dir) {
        list_directory(target_path);
    }
    else {
        clear_data();
        is_wildcard = (strchr(query.name, '*') || strchr(query.name, '?'));
        for (int i = 0; i < THREAD_COUNT; i++) 
            _beginthreadex(NULL, 0, hunter_thread, (void*)(intptr_t)search_generation, 0, NULL);
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
    add_favorite(p); // Add to recent/favorites
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

void navigate_down() {
    EnterCriticalSection(&data_lock);
    if (entry_count > 0 && selected_index >= 0 && selected_index < entry_count) {
        Entry *e = &entries[selected_index];
        if (e->is_dir) {
            strcpy(root_path, e->path);
            search_buffer[0] = 0;
            if(!e->is_drive) add_favorite(e->path); // Add folders to recent too
            LeaveCriticalSection(&data_lock);
            refresh_state();
        } else {
            char p[4096]; strcpy(p, e->path);
            LeaveCriticalSection(&data_lock);
            open_path(p);
        }
    } else {
        LeaveCriticalSection(&data_lock);
    }
}

void handle_ctrl_o() {
    char target[4096] = {0};
    EnterCriticalSection(&data_lock);
    if (entry_count > 0 && selected_index >= 0 && selected_index < entry_count) {
        Entry *e = &entries[selected_index];
        if (e->is_dir) strcpy(target, e->path);
        else {
            strcpy(target, e->path);
            char *slash = strrchr(target, '\\');
            if(slash) *slash=0;
        }
    }
    LeaveCriticalSection(&data_lock);
    
    if (!target[0] && root_path[0]) strcpy(target, root_path);
    if (!target[0]) return;

    char args[8192];
    switch (g_ctrl_o_mode) {
        case CTRL_O_WT: snprintf(args, 8192, "-d \"%s\"", target); ShellExecuteA(NULL, "open", "wt.exe", args, target, SW_SHOWDEFAULT); break;
        case CTRL_O_CMD: snprintf(args, 8192, "/K cd /d \"%s\"", target); ShellExecuteA(NULL, "open", "cmd.exe", args, target, SW_SHOWDEFAULT); break;
        case CTRL_O_EXPLORER: ShellExecuteA(NULL, "open", "explorer.exe", target, NULL, SW_SHOWNORMAL); break;
    }
}

int hit_test_row(int x, int y) {
    if (y < HEADER_HEIGHT + 5) return -1;
    int row = (y - (HEADER_HEIGHT + 5)) / ROW_HEIGHT;
    if (row < 0) return -1;
    EnterCriticalSection(&data_lock);
    int idx = scroll_offset + row;
    if (idx < 0 || idx >= (int)entry_count) idx = -1;
    LeaveCriticalSection(&data_lock);
    return idx;
}

// ==========================================
// INPUT BOX & RENAME LOGIC
// ==========================================
static char ib_result[MAX_PATH];
static char ib_prompt[64];
static int ib_success = 0;

LRESULT CALLBACK InputBoxWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE:
            CreateWindow("STATIC", ib_prompt, WS_VISIBLE | WS_CHILD, 10, 10, 280, 20, hwnd, NULL, NULL, NULL);
            HWND hEdit = CreateWindow("EDIT", ib_result, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 10, 35, 280, 25, hwnd, (HMENU)100, NULL, NULL);
            SendMessage(hEdit, EM_SETSEL, 0, -1);
            SetFocus(hEdit);
            CreateWindow("BUTTON", "OK", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 105, 75, 80, 25, hwnd, (HMENU)IDOK, NULL, NULL);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                GetWindowText(GetDlgItem(hwnd, 100), ib_result, MAX_PATH);
                ib_success = 1;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int InputBox(HWND owner, const char *title, const char *prompt, char *buffer) {
    WNDCLASSA wc = {0}; wc.lpfnWndProc = InputBoxWndProc; wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "BladeInputBox"; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1); RegisterClassA(&wc);

    strncpy(ib_result, buffer, MAX_PATH); strncpy(ib_prompt, prompt, 64); ib_success = 0;
    RECT rcOwner; GetWindowRect(owner, &rcOwner);
    int x = rcOwner.left + (rcOwner.right - rcOwner.left)/2 - 150;
    int y = rcOwner.top + (rcOwner.bottom - rcOwner.top)/2 - 75;

    HWND hDlg = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, "BladeInputBox", title, WS_VISIBLE | WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, 320, 150, owner, NULL, GetModuleHandle(NULL), NULL);
    EnableWindow(owner, FALSE);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    EnableWindow(owner, TRUE); SetForegroundWindow(owner);
    if (ib_success) strcpy(buffer, ib_result);
    return ib_success;
}

void rename_entry() {
    char old_path[4096] = {0}; char old_name[MAX_PATH] = {0};
    EnterCriticalSection(&data_lock);
    if (entry_count > 0 && selected_index >= 0 && selected_index < entry_count) {
        lstrcpynA(old_path, entries[selected_index].path, 4096);
        lstrcpynA(old_name, get_display_name(old_path), MAX_PATH);
    }
    LeaveCriticalSection(&data_lock);

    if (old_path[0] == 0) return;
    char new_name[MAX_PATH]; strcpy(new_name, old_name);

    if (InputBox(hMainWnd, "Rename", "Enter new name:", new_name)) {
        if (strcmp(old_name, new_name) == 0) return;
        char new_path[4096]; strcpy(new_path, old_path);
        char *slash = strrchr(new_path, '\\'); if (slash) *(slash+1) = 0;
        strcat(new_path, new_name);
        if (MoveFileA(old_path, new_path)) refresh_state();
        else MessageBoxA(hMainWnd, "Failed to rename file.", "Error", MB_ICONERROR);
    }
}

// ==========================================
// FILE OPERATIONS
// ==========================================
void delete_to_recycle(const char *path) {
    if (!path || !*path) return;
    char from[4096 + 2]; lstrcpynA(from, path, 4096);
    size_t len = strlen(from); from[len] = 0; from[len+1] = 0;
    SHFILEOPSTRUCTA op = {0}; op.hwnd = hMainWnd; op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_ALLOWUNDO|FOF_NOCONFIRMATION|FOF_SILENT|FOF_NOERRORUI;
    SHFileOperationA(&op);
}

void set_copy_from_selection() {
    EnterCriticalSection(&data_lock);
    if (entry_count>0 && selected_index>=0 && selected_index<entry_count) {
        Entry *e = &entries[selected_index];
        lstrcpynA(g_copy_source, e->path, 4096);
        g_copy_is_dir = e->is_dir;
    }
    LeaveCriticalSection(&data_lock);
}

void copy_to_current_dir_from_buffer() {
    if (!root_path[0] || !g_copy_source[0]) return;
    char from[4096+2]; lstrcpynA(from, g_copy_source, 4096);
    size_t len = strlen(from); from[len]=0; from[len+1]=0;
    SHFILEOPSTRUCTA op = {0}; op.hwnd = hMainWnd; op.wFunc = FO_COPY; op.pFrom = from; op.pTo = root_path; op.fFlags = FOF_NOCONFIRMMKDIR|FOF_NOCONFIRMATION|FOF_NOERRORUI;
    SHFileOperationA(&op);
    refresh_state();
}

void create_new_folder() {
    if (!root_path[0]) return;
    char base[]="New Folder"; char full[4096]; int index=0;
    for(;;) {
        if(index==0) snprintf(full,4096,"%s\\%s",root_path,base);
        else snprintf(full,4096,"%s\\%s (%d)",root_path,base,index);
        if(GetFileAttributesA(full)==INVALID_FILE_ATTRIBUTES) break;
        if(++index>1000) return;
    }
    if(CreateDirectoryA(full,NULL)) {
        refresh_state();
        EnterCriticalSection(&data_lock);
        for(long i=0; i<entry_count; ++i) {
            if(_stricmp(entries[i].path, full)==0) { selected_index=(int)i; break; }
        }
        LeaveCriticalSection(&data_lock);
        InvalidateRect(hMainWnd, NULL, FALSE);
    }
}

void show_context_menu(int x, int y) {
    POINT pt = {x,y}; ClientToScreen(hMainWnd, &pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuA(hMenu, MF_STRING, CMD_OPEN, "Open");
    AppendMenuA(hMenu, MF_STRING, CMD_OPEN_EXPLORER, "Open in Explorer");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, CMD_NEW_FOLDER, "New Folder");
    AppendMenuA(hMenu, MF_STRING, CMD_RENAME_ENTRY, "Rename (F2)");
    AppendMenuA(hMenu, MF_STRING, CMD_COPY_ENTRY, "Copy");
    AppendMenuA(hMenu, MF_STRING|(g_copy_source[0]?0:MF_GRAYED), CMD_PASTE_ENTRY, "Paste");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, CMD_DELETE_ENTRY, "Delete");
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON|TPM_LEFTALIGN, pt.x, pt.y, 0, hMainWnd, NULL);
    DestroyMenu(hMenu);
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
    if (strlen(search_buffer) > 0) snprintf(prompt, 300, "Query: %s", search_buffer);
    else strcpy(prompt, "Type to hunt... (e.g. log ext:.txt >1mb)");
    SelectObject(hdcBack, hFontSmall);
    TextOutA(hdcBack, 10, 35, prompt, strlen(prompt));

    int header_y = HEADER_HEIGHT - 20;
    SetTextColor(hdcBack, COL_DIM);
    TextOutA(hdcBack, window_width - 350, header_y, "Name (F3)", 9);
    TextOutA(hdcBack, window_width - 220, header_y, "Size (F4)", 9);
    TextOutA(hdcBack, window_width - 120, header_y, "Date (F5)", 9);

    char stats[128];
    snprintf(stats, 128, "%ld items %s %s", entry_count, active_workers > 0 ? "[BUSY]" : "", is_truncated ? "[LIMIT]" : "");
    SIZE sz; GetTextExtentPoint32A(hdcBack, stats, strlen(stats), &sz);
    TextOutA(hdcBack, window_width - sz.cx - 10, 10, stats, strlen(stats));

    EnterCriticalSection(&data_lock);
    int start_y = HEADER_HEIGHT + 5;
    list_start_y = start_y; 
    max_visible_items = (window_height - start_y) / ROW_HEIGHT;

    if (selected_index >= entry_count) selected_index = entry_count - 1;
    if (selected_index < 0) selected_index = 0;
    if (selected_index < scroll_offset) scroll_offset = selected_index;
    if (selected_index >= scroll_offset + max_visible_items) scroll_offset = selected_index - max_visible_items + 1;

    
    for (int i = 0; i < max_visible_items; i++) {
        int idx = scroll_offset + i;
        if (idx >= entry_count) break;
        int y = start_y + (i * ROW_HEIGHT);
        Entry *e = &entries[idx];

        RECT rcRow = {10, y, window_width - 10, y + ROW_HEIGHT};

        if (idx == selected_index) {
            HBRUSH hSel = CreateSolidBrush(COL_ACCENT);
            FillRect(hdcBack, &rcRow, hSel);
            DeleteObject(hSel);
            SetTextColor(hdcBack, COL_SEL_TEXT);
        } else {
            if (idx == hover_index) {
                HBRUSH hHover = CreateSolidBrush(COL_HOVER);
                FillRect(hdcBack, &rcRow, hHover);
                DeleteObject(hHover);
            }
            if (e->is_recycled) SetTextColor(hdcBack, COL_RECYCLED);
            else SetTextColor(hdcBack, e->is_dir ? COL_DIR : COL_TEXT);
        }

        // Strikethrough for Recycled items
        if (e->is_recycled) SelectObject(hdcBack, hFontStrike);
        else SelectObject(hdcBack, hFont);

        char disp[4096];
        if (e->is_favorite) snprintf(disp, 4096, "* %s", get_display_name(e->path));
        else strcpy(disp, get_display_name(e->path));
        TextOutA(hdcBack, 15, y, disp, strlen(disp));

        // Restore normal font for metadata
        SelectObject(hdcBack, hFont);

        char meta[128] = {0};
        if (e->is_drive) {
            char szFree[32], szTot[32];
            format_size(e->free_bytes, szFree); format_size(e->total_bytes, szTot);
            snprintf(meta, 128, "%s free / %s (%s)", szFree, szTot, e->fs_name);
        } else if (!e->is_dir) {
            format_size(e->size, meta);
        } else if (e->is_favorite) {
            strcpy(meta, "Recent/Favorite");
        }
        
        if (meta[0]) {
            SIZE ssz; GetTextExtentPoint32A(hdcBack, meta, strlen(meta), &ssz);
            TextOutA(hdcBack, window_width - ssz.cx - 15, y, meta, strlen(meta));
        }
    }
    LeaveCriticalSection(&data_lock);

    // HELP OVERLAY
    if (show_help) {
        RECT rcHelp = {window_width/2 - 250, window_height/2 - 200, window_width/2 + 250, window_height/2 + 200};
        HBRUSH hHelpBg = CreateSolidBrush(COL_HELP_BG);
        FillRect(hdcBack, &rcHelp, hHelpBg);
        DeleteObject(hHelpBg);
        FrameRect(hdcBack, &rcHelp, CreateSolidBrush(COL_ACCENT)); 
        
        SetTextColor(hdcBack, COL_TEXT);
        SelectObject(hdcBack, hFontMono);
        const char *lines[] = {
            " BLADE EXPLORER HELP (Ctrl+H)",
            " ----------------------------",
            " Navigation: Arrows, PageUp/Dn, Enter, Backspace",
            " Search:     Type to hunt recursively",
            " Filters:    ext:.log  >100mb  <5kb",
            " Favorites:  Saved automatically on open",
            " Ops:        Ctrl+C/V (Copy/Paste)",
            "             F2 (Rename), Del (Recycle Bin)",
            "             Ctrl+Shift+N (New Folder)",
            " Sort:       F3 (Name), F4 (Size), F5 (Date)",
            " Power:      Ctrl+L (Copy Current Path)",
            "             Ctrl+O (Open Here - configurable)",
            "             Ctrl+Enter (Open in Explorer)"
        };
        for(int i=0; i<13; i++) TextOutA(hdcBack, rcHelp.left + 20, rcHelp.top + 20 + (i*25), lines[i], strlen(lines[i]));
    }

    BitBlt(hdcDest, 0, 0, window_width, window_height, hdcBack, 0, 0, SRCCOPY);
}

void header_click(int x, int y) {
    int header_y = HEADER_HEIGHT - 20;
    if (y < header_y - 5 || y > header_y + 20) return;
    if (x < window_width - 250) g_sort_mode = SORT_NAME;
    else if (x < window_width - 150) g_sort_mode = SORT_SIZE;
    else g_sort_mode = SORT_DATE;
    sort_entries();
}

// ==========================================
// WINDOW PROC
// ==========================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE:
            hFont = CreateFontA(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
            hFontSmall = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
            hFontMono = CreateFontA(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");
            // Strikethrough Font
            hFontStrike = CreateFontA(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, TRUE /* Strike */, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
            
            CoInitialize(NULL); // Required for SHGetKnownFolderPath
            load_settings();
            load_favorites();
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
        
        case WM_MOUSEMOVE: {
            int idx = hit_test_row(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (idx != hover_index) { hover_index = idx; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            if (show_help) { show_help = 0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            int x = GET_X_LPARAM(lParam); int y = GET_Y_LPARAM(lParam);
            if (y < HEADER_HEIGHT) { header_click(x, y); return 0; }
            int idx = hit_test_row(x, y);
            if (idx >= 0) { selected_index = idx; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int idx = hit_test_row(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (idx >= 0) {
                selected_index = idx; InvalidateRect(hwnd, NULL, FALSE);
                navigate_down();
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            int x = GET_X_LPARAM(lParam); int y = GET_Y_LPARAM(lParam);
            int idx = hit_test_row(x, y);
            if (idx >= 0) { selected_index = idx; InvalidateRect(hwnd, NULL, FALSE); }
            show_context_menu(x, y);
            return 0;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case CMD_OPEN: navigate_down(); break;
                case CMD_OPEN_EXPLORER: {
                    EnterCriticalSection(&data_lock);
                    if (entry_count > 0 && selected_index >= 0 && selected_index < entry_count) open_in_explorer(entries[selected_index].path);
                    LeaveCriticalSection(&data_lock);
                    break;
                }
                case CMD_NEW_FOLDER: create_new_folder(); break;
                case CMD_RENAME_ENTRY: rename_entry(); break;
                case CMD_COPY_ENTRY: set_copy_from_selection(); break;
                case CMD_PASTE_ENTRY: copy_to_current_dir_from_buffer(); break;
                case CMD_DELETE_ENTRY: {
                    char path[4096] = {0};
                    EnterCriticalSection(&data_lock);
                    if (entry_count > 0 && selected_index >= 0 && selected_index < entry_count) lstrcpynA(path, entries[selected_index].path, 4096);
                    LeaveCriticalSection(&data_lock);
                    if (path[0]) { delete_to_recycle(path); refresh_state(); }
                    break;
                }
            }
            return 0;
        }

        case WM_CHAR:
            if (show_help) { show_help=0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == VK_ESCAPE) { if (strlen(search_buffer)>0) { search_buffer[0]=0; refresh_state(); } else PostQuitMessage(0); }
            else if (wParam == VK_BACK) { int l=strlen(search_buffer); if(l>0) search_buffer[l-1]=0; else navigate_up(); refresh_state(); }
            else if (wParam >= 32 && wParam < 127 && strlen(search_buffer) < 250) { int l=strlen(search_buffer); search_buffer[l]=(char)wParam; search_buffer[l+1]=0; refresh_state(); }
            return 0;
        case WM_KEYDOWN:
            if (show_help) { show_help=0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
            switch(wParam) {
                case VK_UP: if (selected_index > 0) selected_index--; break;
                case VK_DOWN: selected_index++; break;
                case VK_PRIOR: selected_index -= max_visible_items; break;
                case VK_NEXT: selected_index += max_visible_items; break;
                case VK_DELETE: SendMessage(hwnd, WM_COMMAND, CMD_DELETE_ENTRY, 0); break;
                case VK_F2: SendMessage(hwnd, WM_COMMAND, CMD_RENAME_ENTRY, 0); break;
                case VK_F3: g_sort_mode = SORT_NAME; sort_entries(); break;
                case VK_F4: g_sort_mode = SORT_SIZE; sort_entries(); break;
                case VK_F5: g_sort_mode = SORT_DATE; sort_entries(); break;
                case VK_RETURN: {
                if (GetKeyState(VK_CONTROL) & 0x8000) { 
                    EnterCriticalSection(&data_lock); 
                    if (entry_count > 0) open_in_explorer(entries[selected_index].path); 
                    LeaveCriticalSection(&data_lock); 
                }
                else { 
                    char target_path[4096] = {0};
                    int is_absolute = (search_buffer[1] == ':' || (search_buffer[0] == '\\' && search_buffer[1] == '\\'));
                    if (is_absolute) lstrcpynA(target_path, search_buffer, 4096);
                    else if (strlen(root_path) > 0 && strlen(search_buffer) > 0) snprintf(target_path, 4096, "%s\\%s", root_path, search_buffer);
                    DWORD attr = (target_path[0]) ? GetFileAttributesA(target_path) : INVALID_FILE_ATTRIBUTES;

                    if (attr != INVALID_FILE_ATTRIBUTES) {
                        if (attr & FILE_ATTRIBUTE_DIRECTORY) { strcpy(root_path, target_path); search_buffer[0] = 0; refresh_state(); } 
                        else open_path(target_path);
                    } 
                    else navigate_down(); 
                }
                return 0;
            }
                case 'C': {
                    SHORT ctrl = GetKeyState(VK_CONTROL); SHORT shift = GetKeyState(VK_SHIFT);
                    if ((ctrl&0x8000) && (shift&0x8000)) { EnterCriticalSection(&data_lock); if(entry_count>0) copy_to_clipboard(entries[selected_index].path); LeaveCriticalSection(&data_lock); } 
                    else if (ctrl&0x8000) SendMessage(hwnd, WM_COMMAND, CMD_COPY_ENTRY, 0);
                    break;
                }
                case 'V': if (GetKeyState(VK_CONTROL)&0x8000) SendMessage(hwnd, WM_COMMAND, CMD_PASTE_ENTRY, 0); break;
                case 'N': if ((GetKeyState(VK_CONTROL)&0x8000) && (GetKeyState(VK_SHIFT)&0x8000)) SendMessage(hwnd, WM_COMMAND, CMD_NEW_FOLDER, 0); break;
                case 'L': if (GetKeyState(VK_CONTROL)&0x8000) copy_to_clipboard(root_path); break;
                case 'H': if (GetKeyState(VK_CONTROL)&0x8000) { show_help = !show_help; InvalidateRect(hwnd, NULL, FALSE); } break;
                case 'O': if (GetKeyState(VK_CONTROL)&0x8000) handle_ctrl_o(); break;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_DESTROY: 
            running=0; KillTimer(hwnd,1); clear_data(); free(entries); 
            DeleteCriticalSection(&data_lock); 
            CoUninitialize(); 
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
    wc.style = CS_DBLCLKS; 
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "BladeClass"; RegisterClassA(&wc);
    hMainWnd = CreateWindowExA(0, "BladeClass", "Blade Explorer", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, NULL, NULL, hInst, NULL);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}
