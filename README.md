
# blade 

A high-performance, AVX2-accelerated file explorer and terminal search engine for Windows.

## 🖥 Modes

Blade ships in two flavors to suit your workflow:

1.  **GUI Explorer (`blade_gui.exe`)**  
    A full GDI file explorer with "Type-to-Hunt" navigation, context menus, and deep shell integration.

2.  **TUI Scanner (`blade.exe`)**  
    A high-speed terminal scanner for recursive search from the command line.

---

# ⚡ GUI Explorer (`blade_gui.exe`)

## Features

*   **AVX2 Acceleration:** Custom SIMD engine for blazing fast case-insensitive substring matching.
*   **Parallel Scanning:** 16-thread work pool for recursive "Type-to-Hunt" searching.
*   **Zero Allocation Search:** Uses a custom Arena Allocator for search strings—no malloc churn on the hot path.
*   **Native GDI GUI:** Double-buffered, responsive interface with standard Windows controls.
*   **Power Search:** Support for wildcards (`*`, `?`) and advanced filters (`ext:`, `>`, `<`).
*   **Shell Integration:** Context menus, Recycle Bin deletion, and copy/paste compatibility with Windows Explorer.

## Usage

Run the executable directly to start in "This PC", or pass a path to start elsewhere:

```cmd
blade_gui.exe
blade_gui.exe C:\Projects
```

### Search Grammar
Blade switches from **Browse Mode** to **Hunter Mode** instantly when you type.

*   **Simple:** `invoice` (matches *invoice* anywhere)
*   **Wildcard:** `*.pdf` or `inv*2024`
*   **Extension:** `ext:.c` or `ext:png`
*   **Size:** `>100mb`, `<5kb`, `>1gb`
*   **Combined:** `driver ext:.sys <1mb`

### ⚙️ Configuration (`blade.ini`)
Create a `blade.ini` file next to the executable to configure the **Ctrl+O** "Open Here" behavior.

```ini
[General]
; Options: wt (Windows Terminal), cmd (Command Prompt), explorer (File Explorer)
CtrlO=wt
```

## ⌨️ GUI Controls

| Key | Action |
| :--- | :--- |
| **Navigation** | |
| `Arrows / PgUp / PgDn` | Navigate list |
| `Enter` | Open file / Enter directory |
| `Backspace` | Go up one level (or delete search text) |
| `Mouse Click` | Select / Navigate (Double Click) |
| **Operations** | |
| `Ctrl + C` | Copy file (for pasting) |
| `Ctrl + V` | Paste file |
| `Del` | Delete to Recycle Bin |
| `Ctrl + Shift + N` | Create New Folder |
| `Right Click` | Open Context Menu |
| **Power User** | |
| `F3 / F4 / F5` | Sort by **Name** / **Size** / **Date** |
| `Ctrl + L` | Copy current folder path to clipboard |
| `Ctrl + Shift + C` | Copy selected file path to clipboard |
| `Ctrl + O` | Open Terminal/Shell here (Configurable) |
| `Ctrl + Enter` | Open selected item in Windows Explorer |
| `Ctrl + H` | Toggle Help Overlay |
| `ESC` | Clear search / Quit |

---

# 🔍 TUI Scanner (`blade.exe`)

The TUI is a non-interactive launcher: you give it a root directory and a search term, it recursively scans, and shows results in a minimal text UI.

## CLI Usage

```cmd
blade.exe <directory> <search_term>
```

### Examples

```cmd
:: Search for "driver" anywhere under C:\
blade.exe C:\ driver

:: Glob pattern on filenames (fast_glob_match):
blade.exe C:\Windows *.sys

:: Case-insensitive substring:
blade.exe C:\Projects report_2024
```

### Notes
1.  **Directory:** Resolved to a full path; trailing backslash is normalized.
2.  **Search Term:**
    *   Supports `*` and `?` wildcard patterns (e.g. `*.log`, `inv??.pdf`).
    *   Otherwise uses an AVX2-accelerated, case-insensitive substring matcher on filenames.

If called with fewer than 2 args, it prints version/usage and exits.

## TUI Interface

The TUI window displays a header and a single list of matching paths.

**Header Layout:**
```text
blade <VERSION> :: Found: <N> (<TOTAL_SIZE>) :: Sel: <SIZE> :: Scanning...|Ready
```

*   **Found:** Number of results in the current view (filtered or full).
*   **TOTAL_SIZE:** AVX2-summed size of all visible entries.
*   **Sel:** Size of the currently selected file.
*   **Status:** `Scanning...` (threads active) or `Ready` (scan complete).

## ⌨️ TUI Controls

| Key | Action |
| :--- | :--- |
| **Navigation** | |
| `↑` / `↓` | Move selection up/down one item |
| `PgUp` / `PgDn` | Jump ±10 items |
| **Open** | |
| `Enter` | Reveal selected item in Windows Explorer (`/select`) |
| **Filtering** | |
| `Ctrl + F` | Toggle Post-Filter Mode on/off |
| `TAB` *(Filter ON)* | Toggle filter field between **Name** and **Path** |
| `Backspace` *(Filter ON)* | Delete last character from filter text |
| `Any Char` *(Filter ON)* | Append character to filter |
| **Quit** | |
| `ESC` *(Filter ON)* | Exit Filter Mode and clear filter |
| `ESC` *(Normal)* | Quit blade |

## Filter Semantics
Filtering is performed **post-scan** on the result set currently in memory. It does not hit the filesystem.

*   **Name Mode:** Matches against the basename only.
*   **Path Mode:** Matches against the full absolute path.
*   **Logic:**
    *   If filter text contains `*` or `?`, it is treated as a glob (`fast_glob_match`).
    *   Otherwise, it is a case-insensitive substring match (`stristr`).

## TUI Performance Model
Under the hood, the TUI scanner:
*   Uses a **dynamic work queue** (`QueueNode`) with condition variables.
*   Spawns **16 worker threads** (default) that:
    1.  Pop directories.
    2.  Enumerate via `FindFirstFileExA` (`FIND_FIRST_EX_LARGE_FETCH`).
    3.  Match filenames against `TARGET_RAW` / `TARGET_LOWER`.
    4.  Batch matches into per-thread buffers, then bulk-commit via `add_results_batch`.
*   Avoids following reparse points (prevents symlink loops).
*   Maintains separate, 32-byte-aligned `file_sizes[]` for fast AVX2 summation in the header.

---

# 🛠️ Building

**Requirements:**
*   Windows Vista+
*   CPU with AVX2 support (Haswell 2013+)
*   MinGW (GCC)

**Build TUI (`blade.exe`):**
```bash
gcc -O3 -mavx2 blade_tui.c -o blade.exe
```

**Build GUI (`blade_gui.exe`):**
```bash
gcc -O3 -mavx2 -mwindows blade_gui.c -o blade_gui.exe -lgdi32 -luser32 -lshell32 -lole32 -lcomctl32
```

## 📄 License
MIT