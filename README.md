
## ⚡ Features

*   **AVX2 Acceleration:** Custom SIMD engine for case-insensitive substring matching.
*   **Parallel Scanning:** 16-thread work pool for maximum I/O throughput.
*   **TUI Viewer:** Navigate results with keyboard, hit Enter to open in Explorer.
*   **Smart Globbing:** Supports `*` and `?` wildcards.
*   **Zero Dependencies:** Single native executable.

## 🚀 Quick Start

### Download
Grab the latest binary from the [Releases](https://github.com/mewmix/frostmourne/releases) page.

### Usage
```cmd
./frostmourne.exe <directory> <search_term>
```

**Examples:**
```cmd
# Simple text search
./frostmourne.exe C:\Projects TODO

# Wildcard search
./frostmourne.exe D:\Downloads invoice_*.pdf
```

### TUI Controls
*   `UP / DOWN` : Navigate results.
*   `ENTER` : Open selected file in Explorer.
*   `ESC` : Quit.

## 🛠️ Building

**Requirements:** Windows, CPU with AVX2 support (Haswell 2013+), MSVC or MinGW.

**GCC (MinGW):**
```bash
gcc -O3 -mavx2 frostmourne_tui.c -o frostmourne.exe
```


## 📄 License
MIT