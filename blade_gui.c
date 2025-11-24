// Compile with: cl blade.c user32.lib gdi32.lib shell32.lib ole32.lib /O2 /arch:AVX2
// Or MinGW: gcc blade.c -o blade.exe -lgdi32 -luser32 -lshell32 -lole32 -mavx2 -O3

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // Force Vista+
#endif

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h> 
#include <knownfolders.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <immintrin.h>

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

// Layout
#define ROW_HEIGHT 28
#define HEADER_HEIGHT 70
#define GRID_ITEM_WIDTH 120
#define GRID_ITEM_HEIGHT 100

// Limits
#define MAX_PINNED 20
#define MAX_HISTORY 5

// Context menu command IDs
#define CMD_OPEN            1001
#define CMD_OPEN_EXPLORER   1002
#define CMD_NEW_FOLDER      1003
#define CMD_COPY_ENTRY      1004
#define CMD_PASTE_ENTRY     1005
#define CMD_DELETE_ENTRY    1006
#define CMD_RENAME_ENTRY    1007
#define CMD_ADD_FAV         1008
#define CMD_REMOVE_FAV      1009
#define CMD_TOGGLE_VIEW     1010

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
// Compile with: cl blade.c user32.lib gdi32.lib shell32.lib ole32.lib /O2 /arch:AVX2
// Or MinGW: gcc blade.c -o blade.exe -lgdi32 -luser32 -lshell32 -lole32 -mavx2 -O3

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // Force Vista+
#endif

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h> 
#include <knownfolders.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <immintrin.h>

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

// Layout
#define ROW_HEIGHT 28
#define HEADER_HEIGHT 70
#define GRID_ITEM_WIDTH 120
#define GRID_ITEM_HEIGHT 100

// Limits
#define MAX_PINNED 20
#define MAX_HISTORY 5

// Context menu command IDs
#define CMD_OPEN            1001
#define CMD_OPEN_EXPLORER   1002
#define CMD_NEW_FOLDER      1003
#define CMD_COPY_ENTRY      1004
#define CMD_PASTE_ENTRY     1005
#define CMD_DELETE_ENTRY    1006
#define CMD_RENAME_ENTRY    1007
#define CMD_ADD_FAV         1008
#define CMD_REMOVE_FAV      1009
#define CMD_TOGGLE_VIEW     1010

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
#define COL_RECYCLED RGB(150, 80, 80)
#define COL_SECTION  RGB(0, 255, 127) // Spring Green for section headers

// ==========================================
// DATA STRUCTURES
// ==========================================
typedef enum { SEC_NONE=0, SEC_CORE, SEC_PINNED, SEC_RECENT, SEC_DRIVES } SECTION_TYPE;

typedef enum { 
    STACK_NONE=0, 
    STACK_TODAY, STACK_YESTERDAY, STACK_THIS_WEEK, STACK_EARLIER_THIS_MONTH, STACK_LAST_MONTH, STACK_OLDER,
    STACK_IMAGES, STACK_PDFS, STACK_ARCHIVES, STACK_DOCS, STACK_AUDIO, STACK_VIDEO, STACK_CODE, STACK_EXEC, STACK_OTHER,
    STACK_WORK, STACK_PERSONAL
} STACK_TYPE;

typedef enum { STACKMODE_NONE=0, STACKMODE_TIME, STACKMODE_TYPE, STACKMODE_CONTEXT } STACK_MODE;


typedef struct {
    char *path;
    int is_dir;
    unsigned long long size;
    FILETIME write_time;
    int is_drive;
    int is_recycled;
    SECTION_TYPE section; // Distinct separation
    STACK_TYPE stack;     // Dynamic grouping
    unsigned long long total_bytes;
    unsigned long long free_bytes;
    char fs_name[8];
} Entry;

typedef enum { SORT_NAME=0, SORT_SIZE, SORT_DATE } SORT_MODE;
// Avoid name clash with Windows headers (e.g., shlobj.h)
typedef enum { VIEWMODE_LIST=0, VIEWMODE_GRID } VIEW_MODE;
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

// Distinct Favorites Lists
char pinned_dirs[MAX_PINNED][MAX_PATH];
int pinned_count = 0;

char history_dirs[MAX_HISTORY][MAX_PATH];
int history_count = 0;

// App State
volatile int running = 1;
volatile long active_workers = 0;
volatile long search_generation = 0;
SORT_MODE g_sort_mode = SORT_NAME;
VIEW_MODE g_view_mode = VIEWMODE_LIST;
STACK_MODE g_stack_mode = STACKMODE_NONE;
CTRL_O_MODE g_ctrl_o_mode = CTRL_O_WT;

char root_path[4096] = {0};
char search_buffer[256] = {0};
char g_ini_path[MAX_PATH] = {0};
int is_wildcard = 0;
int show_help = 0;

// Copy/paste state
char g_copy_source[4096] = {0};
int  g_copy_is_dir = 0;

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
int items_per_row = 1; // For Grid
int is_truncated = 0;

// GDI Resources
HDC hdcBack = NULL;
HBITMAP hbmBack = NULL;
HFONT hFont = NULL;
HFONT hFontSmall = NULL;
HFONT hFontMono = NULL;
HFONT hFontStrike = NULL;
HFONT hFontBold = NULL;

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
    char *tok = strtok(raw, " ");
    while (tok) {
        if (strncmp(tok, "ext:", 4) == 0) {
            strncpy(query.ext, tok+4, 15);
            for(int i=0; query.ext[i]; i++) query.ext[i] = tolower(query.ext[i]);
        } else if (tok[0] == '>') query.min_size = parse_size_str(tok+1);
        else if (tok[0] == '<') query.max_size = parse_size_str(tok+1);
        else {
            if (query.name[0]) strcat(query.name, " ");
            strcat(query.name, tok);
        }
        tok = strtok(NULL, " ");
    }
    for(int i=0; query.name[i]; i++) query.name[i] = tolower(query.name[i]);
}

// ==========================================
// STACKS LOGIC
// ==========================================
const char* get_stack_name(STACK_TYPE type) {
    switch(type) {
        case STACK_TODAY: return "Today";
        case STACK_YESTERDAY: return "Yesterday";
        case STACK_THIS_WEEK: return "This Week";
        case STACK_EARLIER_THIS_MONTH: return "Earlier This Month";
        case STACK_LAST_MONTH: return "Last Month";
        case STACK_OLDER: return "Older";
        case STACK_IMAGES: return "Images";
        case STACK_PDFS: return "PDFs";
        case STACK_ARCHIVES: return "Archives";
        case STACK_DOCS: return "Documents";
        case STACK_AUDIO: return "Audio";
        case STACK_VIDEO: return "Video";
        case STACK_CODE: return "Code";
        case STACK_EXEC: return "Executables";
        case STACK_WORK: return "Work";
        case STACK_PERSONAL: return "Personal";
        case STACK_OTHER: return "Other";
        default: return "Unsorted";
    }
}

STACK_TYPE get_stack_type(const Entry *e, STACK_MODE mode) {
    if (mode == STACKMODE_NONE) return STACK_NONE;
    
    if (mode == STACKMODE_TIME) {
        SYSTEMTIME st; GetLocalTime(&st);
        FILETIME ftNow; SystemTimeToFileTime(&st, &ftNow);
        ULARGE_INTEGER ulNow = {ftNow.dwLowDateTime, ftNow.dwHighDateTime};
        ULARGE_INTEGER ulFile = {e->write_time.dwLowDateTime, e->write_time.dwHighDateTime};
        
        unsigned long long diff = ulNow.QuadPart - ulFile.QuadPart;
        unsigned long long one_day = 864000000000ULL; // 100ns intervals
        
        if (diff < one_day) return STACK_TODAY;
        if (diff < one_day * 2) return STACK_YESTERDAY;
        if (diff < one_day * 7) return STACK_THIS_WEEK;
        if (diff < one_day * 30) return STACK_EARLIER_THIS_MONTH; // Rough approx
        if (diff < one_day * 60) return STACK_LAST_MONTH;
        return STACK_OLDER;
    }
    
    if (mode == STACKMODE_TYPE) {
        if (e->is_dir) return STACK_OTHER;
        const char *ext = strrchr(e->path, '.');
        if (!ext) return STACK_OTHER;
        
        if (stristr(ext, ".png") || stristr(ext, ".jpg") || stristr(ext, ".jpeg") || stristr(ext, ".gif") || stristr(ext, ".bmp") || stristr(ext, ".webp")) return STACK_IMAGES;
        if (stristr(ext, ".pdf")) return STACK_PDFS;
        if (stristr(ext, ".zip") || stristr(ext, ".rar") || stristr(ext, ".7z") || stristr(ext, ".tar") || stristr(ext, ".gz")) return STACK_ARCHIVES;
        if (stristr(ext, ".doc") || stristr(ext, ".docx") || stristr(ext, ".txt") || stristr(ext, ".rtf") || stristr(ext, ".odt")) return STACK_DOCS;
        if (stristr(ext, ".mp3") || stristr(ext, ".wav") || stristr(ext, ".flac") || stristr(ext, ".ogg")) return STACK_AUDIO;
        if (stristr(ext, ".mp4") || stristr(ext, ".mkv") || stristr(ext, ".avi") || stristr(ext, ".mov")) return STACK_VIDEO;
        if (stristr(ext, ".c") || stristr(ext, ".h") || stristr(ext, ".cpp") || stristr(ext, ".py") || stristr(ext, ".js") || stristr(ext, ".html") || stristr(ext, ".css") || stristr(ext, ".json")) return STACK_CODE;
        if (stristr(ext, ".exe") || stristr(ext, ".msi") || stristr(ext, ".bat") || stristr(ext, ".cmd") || stristr(ext, ".ps1")) return STACK_EXEC;
        
        return STACK_OTHER;
    }

    if (mode == STACKMODE_CONTEXT) {
        // Simple heuristic based on path keywords
        if (stristr(e->path, "Work") || stristr(e->path, "Project") || stristr(e->path, "Office") || stristr(e->path, "Client")) return STACK_WORK;
        if (stristr(e->path, "Personal") || stristr(e->path, "Game") || stristr(e->path, "Photo") || stristr(e->path, "Music")) return STACK_PERSONAL;
        return STACK_OTHER;
    }

    return STACK_NONE;
}

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
        free(curr->data); free(curr);
        curr = next;
    }
    arena_head = NULL;
}

// ==========================================
// FAVORITES / HISTORY MANAGEMENT
// ==========================================
void get_config_path(char *out_path) {
    PWSTR localAppData = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &localAppData))) {
        wcstombs(out_path, localAppData, MAX_PATH);
        CoTaskMemFree(localAppData);
        strcat(out_path, "\\BladeExplorer");
        CreateDirectoryA(out_path, NULL);
        strcat(out_path, "\\blade_data.dat");
    } else {
        strcpy(out_path, "blade_data.dat");
    }
}

void save_data() {
    char path[MAX_PATH]; get_config_path(path);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(&pinned_count, sizeof(int), 1, f);
        for (int i=0; i<pinned_count; i++) fwrite(pinned_dirs[i], 1, MAX_PATH, f);
        fwrite(&history_count, sizeof(int), 1, f);
        for (int i=0; i<history_count; i++) fwrite(history_dirs[i], 1, MAX_PATH, f);
        fclose(f);
    }
}

void load_data() {
    char path[MAX_PATH]; get_config_path(path);
    FILE *f = fopen(path, "rb");
    if (f) {
        if (fread(&pinned_count, sizeof(int), 1, f)) {
            if (pinned_count > MAX_PINNED) pinned_count = MAX_PINNED;
            for (int i=0; i<pinned_count; i++) fread(pinned_dirs[i], 1, MAX_PATH, f);
        }
        if (fread(&history_count, sizeof(int), 1, f)) {
            if (history_count > MAX_HISTORY) history_count = MAX_HISTORY;
            for (int i=0; i<history_count; i++) fread(history_dirs[i], 1, MAX_PATH, f);
        }
        fclose(f);
    }
}

void add_to_history(const char *path) {
    if (path[1] != ':') return; // Don't add relative or virtual paths
    // Remove if exists
    for (int i=0; i<history_count; i++) {
        if (_stricmp(history_dirs[i], path) == 0) {
            for (int k=i; k>0; k--) strcpy(history_dirs[k], history_dirs[k-1]);
            strcpy(history_dirs[0], path);
            return;
        }
    }
    // Shift down
    if (history_count < MAX_HISTORY) history_count++;
    for (int i = history_count - 1; i > 0; i--) strcpy(history_dirs[i], history_dirs[i-1]);
    strncpy(history_dirs[0], path, MAX_PATH);
    save_data();
}

void pin_favorite(const char *path) {
    for (int i=0; i<pinned_count; i++) if (_stricmp(pinned_dirs[i], path)==0) return;
    if (pinned_count < MAX_PINNED) {
        strncpy(pinned_dirs[pinned_count++], path, MAX_PATH);
        save_data();
    }
}

void remove_favorite(const char *path) {
    int found = -1;
    for (int i=0; i<pinned_count; i++) if (_stricmp(pinned_dirs[i], path)==0) found = i;
    if (found != -1) {
        for (int i=found; i<pinned_count-1; i++) strcpy(pinned_dirs[i], pinned_dirs[i+1]);
        pinned_count--;
        save_data();
    }
}

int is_pinned(const char *path) {
    for (int i=0; i<pinned_count; i++) if (_stricmp(pinned_dirs[i], path)==0) return 1;
    return 0;
}

void load_settings() {
    GetModuleFileNameA(NULL, g_ini_path, MAX_PATH);
    char *last = strrchr(g_ini_path, '\\'); if (last) *(last+1)=0;
    strcat(g_ini_path, "blade.ini");
    char buf[32]={0};
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
    entry_count = 0; selected_index = 0; scroll_offset = 0; is_truncated = 0;
    LeaveCriticalSection(&data_lock);
}

void add_entry_ex(const char *full, int dir, unsigned long long sz, const FILETIME *ft, 
                 int is_drive, SECTION_TYPE sec, unsigned long long tot, unsigned long long free_b, const char *fs) {
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
    e->is_recycled = (stristr(full, "$Recycle.Bin") || stristr(full, "\\RECYCLER\\")) ? 1 : 0;
    e->section = sec;
    if (ft) e->write_time = *ft; else memset(&e->write_time, 0, sizeof(FILETIME));
    e->is_drive = is_drive;
    e->stack = get_stack_type(e, g_stack_mode); // Assign stack on creation
    if (is_drive) {
        e->total_bytes = tot; e->free_bytes = free_b;
        strncpy(e->fs_name, fs ? fs : "", 7);
    }
    LeaveCriticalSection(&data_lock);
}

void update_stacks() {
    EnterCriticalSection(&data_lock);
    for (int i = 0; i < entry_count; i++) {
        entries[i].stack = get_stack_type(&entries[i], g_stack_mode);
    }
    LeaveCriticalSection(&data_lock);
}

// ==========================================
// MATCHING & SORTING
// ==========================================
int fast_glob_match(const char *text, const char *pattern) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1;
            while (*text) { if (fast_glob_match(text, pattern)) return 1; text++; }
            return 0;
        }
        if (tolower((unsigned char)*text) != tolower((unsigned char)*pattern) && *pattern != '?') return 0;
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
    for (size_t i = 0; haystack[i]; i += 32) {
        __m256i block = _mm256_loadu_si256((const __m256i*)(haystack + i));
        __m256i block_lower = _mm256_or_si256(block, vec_case_mask);
        unsigned int mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(vec_first, block_lower));
        while (mask) {
            int bit_pos = __builtin_ctz(mask);
            if (_strnicmp(haystack + i + bit_pos, needle, needle_len) == 0) return 1;
            mask &= ~(1 << bit_pos);
        }
    }
    return 0;
}

static const char* get_display_name(const char *path) {
    const char *slash = strrchr(path, '\\');
    return (slash && slash[1] != '\0') ? slash + 1 : path;
}

int __cdecl entry_cmp(const void *pa, const void *pb) {
    const Entry *a = (const Entry*)pa;
    const Entry *b = (const Entry*)pb;
    
    // Sort by section first
    if (a->section != b->section) return a->section - b->section;

    // Sort by Stack if active
    if (g_stack_mode != STACKMODE_NONE) {
        if (a->stack != b->stack) return a->stack - b->stack;
    }

    // Folders before files
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;

    switch (g_sort_mode) {
        case SORT_SIZE: if (a->size != b->size) return (b->size > a->size) ? 1 : -1; break;
        case SORT_DATE: { LONG c = CompareFileTime(&a->write_time, &b->write_time); if (c!=0) return -c; break; }
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
            add_entry_ex(full, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), sz, &fd.ftLastWriteTime, 0, SEC_NONE, 0, 0, NULL);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    sort_entries();
}

void add_core_folder(REFKNOWNFOLDERID rfid, const char* label) {
    PWSTR wpath = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, NULL, &wpath))) {
        char path[MAX_PATH]; wcstombs(path, wpath, MAX_PATH);
        CoTaskMemFree(wpath);
        // We use the path as the "display" here but render the label later
        // Hack: Append label to path in a special way or just use raw path
        add_entry_ex(path, 1, 0, NULL, 0, SEC_CORE, 0,0, NULL);
    }
}

void list_home_view() {
    clear_data();
    
    // 1. Core User Directories
    add_core_folder(&FOLDERID_Downloads, "Downloads");
    add_core_folder(&FOLDERID_Desktop, "Desktop");
    add_core_folder(&FOLDERID_Documents, "Documents");
    add_core_folder(&FOLDERID_Pictures, "Pictures");
    add_core_folder(&FOLDERID_Videos, "Videos");
    add_core_folder(&FOLDERID_Music, "Music");

    // 2. Favorites (Pinned)
    for(int i=0; i<pinned_count; i++) 
        add_entry_ex(pinned_dirs[i], 1, 0, NULL, 0, SEC_PINNED, 0,0, NULL);

    // 3. Recent (History)
    for(int i=0; i<history_count; i++)
        add_entry_ex(history_dirs[i], 1, 0, NULL, 0, SEC_RECENT, 0,0, NULL);

    // 4. Drives
    DWORD drives = GetLogicalDrives();
    char root[] = "A:\\";
    for (int i = 0; i < 26; i++) {
        if (drives & (1 << i)) {
            root[0] = 'A' + i;
            ULARGE_INTEGER freeBytes, totalBytes, totalFree;
            char fs[16] = {0};
            GetDiskFreeSpaceExA(root, &freeBytes, &totalBytes, &totalFree);
            GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL, fs, 16);
            add_entry_ex(root, 1, 0, NULL, 1, SEC_DRIVES, totalBytes.QuadPart, totalFree.QuadPart, fs);
        }
    }
    // Sort logic handles grouping by Section ID
    sort_entries(); 
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
        else match = is_wildcard ? fast_glob_match(fd.cFileName, query.name) : avx2_strcasestr(fd.cFileName, query.name, name_len);

        if (match && query.ext[0]) {
            const char *dot = strrchr(fd.cFileName, '.');
            if (!dot || _stricmp(dot, query.ext) != 0) match = 0;
        }
        unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (match && query.min_size && sz < query.min_size) match = 0;
        if (match && query.max_size && sz > query.max_size) match = 0;

        char full[4096]; snprintf(full, 4096, "%s\\%s", path, fd.cFileName);
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (match) add_entry_ex(full, is_dir, sz, &fd.ftLastWriteTime, 0, SEC_NONE, 0, 0, NULL);
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
    if (is_absolute) lstrcpynA(target_path, search_buffer, 4096);
    else if (strlen(root_path) > 0) snprintf(target_path, 4096, "%s\\%s", root_path, search_buffer);
    
    DWORD attr = (target_path[0]) ? GetFileAttributesA(target_path) : INVALID_FILE_ATTRIBUTES;
    int is_valid_dir = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));

    if (strlen(search_buffer) == 0) {
        if (strlen(root_path) == 0) list_home_view();
        else list_directory(root_path);
    } else if (is_valid_dir) list_directory(target_path);
    else {
        clear_data();
        is_wildcard = (strchr(query.name, '*') || strchr(query.name, '?'));
        for (int i = 0; i < THREAD_COUNT; i++) _beginthreadex(NULL, 0, hunter_thread, (void*)(intptr_t)search_generation, 0, NULL);
    }
    InvalidateRect(hMainWnd, NULL, FALSE);
}

// ==========================================
// NAVIGATION & ACTIONS
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

void navigate_down() {
    EnterCriticalSection(&data_lock);
    if (entry_count > 0 && selected_index >= 0 && selected_index < entry_count) {
        Entry *e = &entries[selected_index];
        if (e->is_dir) {
            strcpy(root_path, e->path);
            search_buffer[0] = 0;
            if(!e->is_drive) add_to_history(e->path); 
            LeaveCriticalSection(&data_lock);
            refresh_state();
        } else {
            char p[4096]; strcpy(p, e->path);
            LeaveCriticalSection(&data_lock);
            open_path(p);
        }
    } else LeaveCriticalSection(&data_lock);
}

void handle_ctrl_o() {
    char target[4096] = {0};
    EnterCriticalSection(&data_lock);
    if (entry_count > 0 && selected_index >= 0 && selected_index < entry_count) {
        Entry *e = &entries[selected_index];
        strcpy(target, e->path);
        if (!e->is_dir) { char *s = strrchr(target, '\\'); if(s) *s=0; }
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

// Input Box for Rename
static char ib_result[MAX_PATH];
LRESULT CALLBACK InputBoxWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE:
            CreateWindow("STATIC", "New Name:", WS_VISIBLE|WS_CHILD, 10, 10, 280, 20, hwnd, NULL, NULL, NULL);
            CreateWindow("EDIT", ib_result, WS_VISIBLE|WS_CHILD|WS_BORDER|ES_AUTOHSCROLL, 10, 35, 280, 25, hwnd, (HMENU)100, NULL, NULL);
            CreateWindow("BUTTON", "OK", WS_VISIBLE|WS_CHILD|BS_DEFPUSHBUTTON, 105, 75, 80, 25, hwnd, (HMENU)IDOK, NULL, NULL);
            return 0;
        case WM_COMMAND: if (LOWORD(wParam)==IDOK) { GetWindowText(GetDlgItem(hwnd, 100), ib_result, MAX_PATH); DestroyWindow(hwnd); } return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
int InputBox(HWND owner, const char *title, char *buffer) {
    strncpy(ib_result, buffer, MAX_PATH);
    WNDCLASSA wc = {0}; wc.lpfnWndProc = InputBoxWndProc; wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "IB"; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1); RegisterClassA(&wc);
    HWND hDlg = CreateWindowExA(WS_EX_DLGMODALFRAME|WS_EX_TOPMOST, "IB", title, WS_VISIBLE|WS_POPUP|WS_CAPTION|WS_SYSMENU, 
        100, 100, 320, 150, owner, NULL, GetModuleHandle(NULL), NULL);
    EnableWindow(owner, FALSE);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    EnableWindow(owner, TRUE); SetForegroundWindow(owner);
    strcpy(buffer, ib_result); return 1;
}

void rename_entry() {
    char path[4096]; 
    EnterCriticalSection(&data_lock);
    if(entry_count>0 && selected_index < entry_count) {
        if (entries[selected_index].is_drive) { LeaveCriticalSection(&data_lock); return; }
        strcpy(path, entries[selected_index].path);
    }
    LeaveCriticalSection(&data_lock);
    char name[MAX_PATH]; strcpy(name, get_display_name(path));
    if (InputBox(hMainWnd, "Rename", name)) {
        char new_p[4096]; strcpy(new_p, path);
        char *s = strrchr(new_p, '\\'); if(s) *(s+1)=0; strcat(new_p, name);
        MoveFileA(path, new_p); refresh_state();
    }
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

void DrawHelp(HDC hdc) {
    if (!show_help) return;
    
    RECT rc = {0, 0, window_width, window_height};
    FillRect(hdc, &rc, CreateSolidBrush(COL_HELP_BG)); // Dark overlay
    
    SetTextColor(hdc, COL_ACCENT);
    SelectObject(hdc, hFontBold);
    TextOutA(hdc, 50, 50, "Blade Explorer - Help", 21);
    
    SetTextColor(hdc, COL_TEXT);
    SelectObject(hdc, hFont);
    
    const char *lines[] = {
        "Navigation:",
        "  Arrows / Mouse : Navigate items",
        "  Enter          : Open folder / Launch file",
        "  Backspace      : Go Up / Back",
        "  Type           : Instant Search (Glob/Fuzzy)",
        "",
        "Commands:",
        "  F2             : Rename selected item",
        "  F3 / F4 / F5   : Sort by Name / Size / Date",
        "  F6             : Toggle Grid / List View",
        "  F7             : Cycle Stacks (Time, Type, Context)",
        "  Del            : Delete item",
        "  Ctrl + O       : Open Terminal Here",
        "  Ctrl + H       : Toggle this Help",
        "  Esc            : Clear Search / Exit Help / Quit"
    };
    
    int y = 100;
    for (int i = 0; i < 15; i++) {
        TextOutA(hdc, 50, y, lines[i], strlen(lines[i]));
        y += 30;
    }
}

void Render(HDC hdcDest) {
    if (!hdcBack) return;
    RECT rc = {0, 0, window_width, window_height};
    FillRect(hdcBack, &rc, CreateSolidBrush(COL_BG));
    RECT rcHead = {0, 0, window_width, HEADER_HEIGHT};
    FillRect(hdcBack, &rcHead, CreateSolidBrush(COL_HEADER));
    SetBkMode(hdcBack, TRANSPARENT);

    SelectObject(hdcBack, hFontBold);
    SetTextColor(hdcBack, COL_ACCENT);
    TextOutA(hdcBack, 10, 5, strlen(root_path) ? root_path : "Home", strlen(root_path) ? strlen(root_path) : 4);

    SetTextColor(hdcBack, COL_TEXT);
    char prompt[300];
    if (strlen(search_buffer) > 0) snprintf(prompt, 300, "Query: %s", search_buffer);
    else strcpy(prompt, "Type to hunt... (Ctrl+H for Help)");
    SelectObject(hdcBack, hFontSmall);
    TextOutA(hdcBack, 10, 35, prompt, strlen(prompt));

    char stats[128];
    const char* stack_modes[] = {"None", "Time", "Type", "Context"};
    snprintf(stats, 128, "%ld items [%s] %s", entry_count, stack_modes[g_stack_mode], g_view_mode==VIEWMODE_GRID ? "[GRID]" : "[LIST]");
    SIZE sz; GetTextExtentPoint32A(hdcBack, stats, strlen(stats), &sz);
    TextOutA(hdcBack, window_width - sz.cx - 10, 10, stats, strlen(stats));

    EnterCriticalSection(&data_lock);
    
    if (g_view_mode == VIEWMODE_LIST) {
        items_per_row = 1;
    } else {
        items_per_row = window_width / GRID_ITEM_WIDTH;
        if (items_per_row < 1) items_per_row = 1;
    }

    if (selected_index >= entry_count) selected_index = entry_count - 1;
    if (selected_index < 0) selected_index = 0;
    
    // Ensure scroll_offset is valid
    if (scroll_offset >= entry_count) scroll_offset = entry_count - 1;
    if (scroll_offset < 0) scroll_offset = 0;

    int y = HEADER_HEIGHT + 5;
    
    // State tracking for headers
    STACK_TYPE current_stack = STACK_NONE;
    SECTION_TYPE current_section = SEC_NONE;
    
    // Initialize state based on previous item if scrolling
    if (scroll_offset > 0 && entry_count > 0) {
        current_stack = entries[scroll_offset-1].stack;
        current_section = entries[scroll_offset-1].section;
    }

    for (int i = scroll_offset; i < entry_count; i++) {
        if (y > window_height) break;
        
        Entry *e = &entries[i];
        
        // Determine if we need a header
        int draw_header = 0;
        char header_text[128] = {0};
        
        // Priority: Sections (Home View) > Stacks
        int use_sections = (strlen(root_path) == 0 && g_stack_mode == STACKMODE_NONE);
        
        if (use_sections) {
            if (i == 0 || e->section != current_section) {
                draw_header = 1;
                current_section = e->section;
                const char* sec_names[] = {"", "Core Folders", "Favorites", "Recent", "Drives"};
                if (e->section < 5) strcpy(header_text, sec_names[e->section]);
                else strcpy(header_text, "Other");
            }
        } else {
            if (i == 0 || e->stack != current_stack) {
                draw_header = 1;
                current_stack = e->stack;
                if (g_stack_mode != STACKMODE_NONE) strcpy(header_text, get_stack_name(e->stack));
                else draw_header = 0; // No headers in flat mode
            }
        }

        // Draw Header
        if (draw_header && header_text[0]) {
            RECT rcH = {0, y, window_width, y + 24};
            FillRect(hdcBack, &rcH, CreateSolidBrush(COL_HOVER));
            SetTextColor(hdcBack, COL_SECTION);
            SelectObject(hdcBack, hFontBold);
            TextOutA(hdcBack, 10, y + 2, header_text, strlen(header_text));
            y += 28;
        }

        // Calculate Item Position
        // Find start of current group (stack or section) to calculate grid position relative to group start
        int start_of_group = i;
        if (use_sections) {
            while (start_of_group > 0 && entries[start_of_group-1].section == e->section) start_of_group--;
        } else {
            while (start_of_group > 0 && entries[start_of_group-1].stack == e->stack) start_of_group--;
        }
        int group_item_index = i - start_of_group;

        int x, w, h;
        if (g_view_mode == VIEWMODE_LIST) {
            x = 10; 
            w = window_width - 20; h = ROW_HEIGHT;
        } else {
            int col = group_item_index % items_per_row;
            x = 10 + (col * GRID_ITEM_WIDTH);
            w = GRID_ITEM_WIDTH - 5; h = GRID_ITEM_HEIGHT - 5;
        }

        RECT rcItem = {x, y, x + w, y + h};

        // Draw selection/hover
        if (i == selected_index) {
            FillRect(hdcBack, &rcItem, CreateSolidBrush(COL_ACCENT));
            SetTextColor(hdcBack, COL_SEL_TEXT);
        } else {
            if (i == hover_index) FillRect(hdcBack, &rcItem, CreateSolidBrush(COL_HOVER));
            SetTextColor(hdcBack, e->is_recycled ? COL_RECYCLED : (e->is_dir ? COL_DIR : COL_TEXT));
        }

        SelectObject(hdcBack, e->is_recycled ? hFontStrike : hFont);
        char disp[MAX_PATH];
        strcpy(disp, get_display_name(e->path));

        if (g_view_mode == VIEWMODE_LIST) {
            TextOutA(hdcBack, x + 5, y, disp, strlen(disp));
            SelectObject(hdcBack, hFontSmall);
            char meta[128] = {0};
            if (e->is_drive) {
                char f[32], t[32]; format_size(e->free_bytes, f); format_size(e->total_bytes, t);
                snprintf(meta, 128, "[%s] %s free of %s", e->fs_name, f, t);
            } else if (!e->is_dir) format_size(e->size, meta);
            
            if (meta[0]) {
                SIZE sz; GetTextExtentPoint32A(hdcBack, meta, strlen(meta), &sz);
                TextOutA(hdcBack, window_width - sz.cx - 20, y, meta, strlen(meta));
            }
            y += ROW_HEIGHT;
        } else {
            // Grid View
            RECT rcIcon = {x + (w-40)/2, y + 10, x + (w-40)/2 + 40, y + 50};
            HBRUSH hIcon = CreateSolidBrush(e->is_dir ? COL_DIR : COL_DIM);
            FrameRect(hdcBack, &rcIcon, hIcon);
            DeleteObject(hIcon);

            SelectObject(hdcBack, hFontSmall);
            RECT rcText = {x, y + 60, x + w, y + h};
            DrawTextA(hdcBack, disp, -1, &rcText, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
            
            // Increment Y only if end of row or next is new group or end of list
            int next_is_new = 0;
            if (i + 1 < entry_count) {
                if (use_sections) next_is_new = (entries[i+1].section != current_section);
                else next_is_new = (entries[i+1].stack != current_stack);
            }
            
            if ((group_item_index % items_per_row) == items_per_row - 1 || next_is_new || i == entry_count - 1) {
                y += GRID_ITEM_HEIGHT;
            }
        }
    }
    LeaveCriticalSection(&data_lock);
    
    if (show_help) DrawHelp(hdcBack);
    
    BitBlt(hdcDest, 0, 0, window_width, window_height, hdcBack, 0, 0, SRCCOPY);
}

int hit_test_index(int x, int y) {
    if (show_help) return -1; // No clicks when help is open
    if (y < HEADER_HEIGHT + 5) return -1;
    
    EnterCriticalSection(&data_lock);
    int cur_y = HEADER_HEIGHT + 5;
    
    STACK_TYPE current_stack = STACK_NONE;
    SECTION_TYPE current_section = SEC_NONE;
    
    if (scroll_offset > 0 && entry_count > 0) {
        current_stack = entries[scroll_offset-1].stack;
        current_section = entries[scroll_offset-1].section;
    }

    for (int i = scroll_offset; i < entry_count; i++) {
        if (cur_y > window_height) break;
        Entry *e = &entries[i];
        
        int draw_header = 0;
        int use_sections = (strlen(root_path) == 0 && g_stack_mode == STACKMODE_NONE);
        
        if (use_sections) {
            if (i == 0 || e->section != current_section) {
                draw_header = 1;
                current_section = e->section;
            }
        } else {
            if (i == 0 || e->stack != current_stack) {
                draw_header = 1;
                current_stack = e->stack;
                if (g_stack_mode == STACKMODE_NONE) draw_header = 0;
            }
        }

        if (draw_header) cur_y += 28;

        int start_of_group = i;
        if (use_sections) {
            while (start_of_group > 0 && entries[start_of_group-1].section == e->section) start_of_group--;
        } else {
            while (start_of_group > 0 && entries[start_of_group-1].stack == e->stack) start_of_group--;
        }
        int group_item_index = i - start_of_group;

        int h = (g_view_mode == VIEWMODE_LIST) ? ROW_HEIGHT : GRID_ITEM_HEIGHT;
        int w = (g_view_mode == VIEWMODE_LIST) ? window_width : GRID_ITEM_WIDTH;
        int item_x = (g_view_mode == VIEWMODE_LIST) ? 0 : 10 + ((group_item_index % items_per_row) * GRID_ITEM_WIDTH);
        
        // Check hit
        if (y >= cur_y && y < cur_y + h) {
            if (g_view_mode == VIEWMODE_LIST) {
                LeaveCriticalSection(&data_lock);
                return i;
            } else {
                if (x >= item_x && x < item_x + w) {
                    LeaveCriticalSection(&data_lock);
                    return i;
                }
            }
        }

        if (g_view_mode == VIEWMODE_LIST) {
            cur_y += ROW_HEIGHT;
        } else {
            int next_is_new = 0;
            if (i + 1 < entry_count) {
                if (use_sections) next_is_new = (entries[i+1].section != current_section);
                else next_is_new = (entries[i+1].stack != current_stack);
            }
            if ((group_item_index % items_per_row) == items_per_row - 1 || next_is_new || i == entry_count - 1) {
                cur_y += GRID_ITEM_HEIGHT;
            }
        }
    }
    LeaveCriticalSection(&data_lock);
    return -1;
}

void show_context_menu(int x, int y) {
    POINT pt = {x,y}; ClientToScreen(hMainWnd, &pt);
    HMENU hMenu = CreatePopupMenu();
    
    int valid_sel = (selected_index >= 0 && selected_index < entry_count);
    Entry *e = valid_sel ? &entries[selected_index] : NULL;

    AppendMenuA(hMenu, MF_STRING, CMD_OPEN, "Open");
    AppendMenuA(hMenu, MF_STRING, CMD_OPEN_EXPLORER, "Open in Explorer");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    
    // Favorites Logic
    if (e && e->is_dir) {
        if (e->section == SEC_PINNED) AppendMenuA(hMenu, MF_STRING, CMD_REMOVE_FAV, "Remove from Favorites");
        else if (!is_pinned(e->path)) AppendMenuA(hMenu, MF_STRING, CMD_ADD_FAV, "Add to Favorites");
    }

    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, CMD_NEW_FOLDER, "New Folder");
    if (!e || !e->is_drive) {
        AppendMenuA(hMenu, MF_STRING, CMD_RENAME_ENTRY, "Rename (F2)");
        AppendMenuA(hMenu, MF_STRING, CMD_COPY_ENTRY, "Copy Path");
        AppendMenuA(hMenu, MF_STRING, CMD_DELETE_ENTRY, "Delete");
    }
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, CMD_TOGGLE_VIEW, "Toggle Grid/List (F6)");

    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON|TPM_LEFTALIGN, pt.x, pt.y, 0, hMainWnd, NULL);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE:
            hFont = CreateFontA(20, 0, 0, 0, FW_NORMAL, 0,0,0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
            hFontBold = CreateFontA(22, 0, 0, 0, FW_BOLD, 0,0,0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
            hFontSmall = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0,0,0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
            hFontStrike = CreateFontA(20, 0, 0, 0, FW_NORMAL, 0,0,1, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
            CoInitialize(NULL);
            load_settings(); load_data();
            refresh_state();
            SetTimer(hwnd, 1, 100, NULL);
            return 0;
        
        case WM_SIZE:
            window_width = LOWORD(lParam); window_height = HIWORD(lParam);
            if (hdcBack) DeleteDC(hdcBack); 
            { HDC h = GetDC(hwnd); hdcBack = CreateCompatibleDC(h); hbmBack = CreateCompatibleBitmap(h, window_width, window_height); SelectObject(hdcBack, hbmBack); ReleaseDC(hwnd, h); }
            break;

        case WM_PAINT: { PAINTSTRUCT ps; HDC h = BeginPaint(hwnd, &ps); Render(h); EndPaint(hwnd, &ps); return 0; }
        case WM_TIMER: if (active_workers > 0) InvalidateRect(hwnd, NULL, FALSE); return 0;
        
        case WM_MOUSEWHEEL: 
            scroll_offset += ((short)HIWORD(wParam) > 0) ? -3 : 3;
            if (scroll_offset < 0) scroll_offset = 0; 
            InvalidateRect(hwnd, NULL, FALSE); return 0;

        case WM_MOUSEMOVE: {
            int idx = hit_test_index(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (idx != hover_index) { hover_index = idx; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            int idx = hit_test_index(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (idx >= 0) { selected_index = idx; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        }
        case WM_LBUTTONDBLCLK: navigate_down(); return 0;
        case WM_RBUTTONUP: {
            int idx = hit_test_index(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (idx >= 0) selected_index = idx;
            show_context_menu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case CMD_OPEN: navigate_down(); break;
                case CMD_OPEN_EXPLORER: if (entry_count) open_in_explorer(entries[selected_index].path); break;
                case CMD_NEW_FOLDER: if(root_path[0]) CreateDirectoryA("New Folder", NULL); refresh_state(); break;
                case CMD_COPY_ENTRY: if (entry_count) copy_to_clipboard(entries[selected_index].path); break;
                case CMD_RENAME_ENTRY: rename_entry(); break;
                case CMD_ADD_FAV: if (entry_count) pin_favorite(entries[selected_index].path); refresh_state(); break;
                case CMD_REMOVE_FAV: if (entry_count) remove_favorite(entries[selected_index].path); refresh_state(); break;
                case CMD_TOGGLE_VIEW: g_view_mode = !g_view_mode; InvalidateRect(hwnd, NULL, FALSE); break;
                case CMD_DELETE_ENTRY: {
                    if (entries[selected_index].is_drive) break;
                    char p[MAX_PATH]; strcpy(p, entries[selected_index].path);
                    p[strlen(p)+1]=0; // double null
                    SHFILEOPSTRUCTA op={0}; op.hwnd=hwnd; op.wFunc=FO_DELETE; op.pFrom=p; op.fFlags=FOF_ALLOWUNDO|FOF_NOCONFIRMATION;
                    SHFileOperationA(&op); refresh_state(); break;
                }
            }
            return 0;

        case WM_KEYDOWN:
            switch(wParam) {
                case VK_UP: selected_index = (selected_index - items_per_row < 0) ? 0 : selected_index - items_per_row; break;
                case VK_DOWN: selected_index += items_per_row; break;
                case VK_LEFT: if (g_view_mode==VIEWMODE_GRID) selected_index--; break;
                case VK_RIGHT: if (g_view_mode==VIEWMODE_GRID) selected_index++; break;
                case VK_RETURN: if (GetKeyState(VK_CONTROL)&0x8000) open_in_explorer(entries[selected_index].path); else navigate_down(); break;
                case VK_F2: rename_entry(); break;
                case VK_F3: g_sort_mode=SORT_NAME; sort_entries(); break;
                case VK_F4: g_sort_mode=SORT_SIZE; sort_entries(); break;
                case VK_F5: g_sort_mode=SORT_DATE; sort_entries(); break;
                case VK_F6: g_view_mode=!g_view_mode; break;
                case VK_F7: 
                    g_stack_mode = (g_stack_mode + 1) % 4; 
                    update_stacks(); 
                    sort_entries(); 
                    break;
                case VK_DELETE: SendMessage(hwnd, WM_COMMAND, CMD_DELETE_ENTRY, 0); break;
                case 'O': if (GetKeyState(VK_CONTROL)&0x8000) handle_ctrl_o(); break;
                case 'H': if (GetKeyState(VK_CONTROL)&0x8000) { show_help = !show_help; InvalidateRect(hwnd, NULL, FALSE); } break;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_CHAR:
            if (wParam == VK_ESCAPE) { 
                if (show_help) { show_help = 0; InvalidateRect(hwnd, NULL, FALSE); }
                else if (strlen(search_buffer)) { search_buffer[0]=0; refresh_state(); } 
                else PostQuitMessage(0); 
            }
            else if (wParam == VK_BACK) { int l=strlen(search_buffer); if (l) search_buffer[l-1]=0; else navigate_up(); refresh_state(); }
            else if (wParam >= 32 && wParam < 127) { int l=strlen(search_buffer); search_buffer[l]=wParam; search_buffer[l+1]=0; refresh_state(); }
            return 0;

        case WM_DESTROY: 
            running=0; KillTimer(hwnd, 1); clear_data(); free(entries);
            DeleteCriticalSection(&data_lock); CoUninitialize(); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) {
    entries = (Entry*)malloc(INITIAL_CAPACITY * sizeof(Entry));
    entry_capacity = INITIAL_CAPACITY;
    InitializeCriticalSection(&data_lock);
    WNDCLASSA wc = {0}; wc.style = CS_DBLCLKS; wc.lpfnWndProc = WndProc; wc.hInstance = h; wc.lpszClassName = "Blade"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    hMainWnd = CreateWindowExA(0, "Blade", "Blade Explorer", WS_OVERLAPPEDWINDOW|WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, NULL, NULL, h, NULL);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}
