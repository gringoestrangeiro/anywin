# anywin architecture & conventions

Goal: `anywin foo.exe -o foo` turns an x86-64 Windows PE executable into a standalone,
statically linked Linux ELF. No Wine, no PE loader at runtime, no runtime DLL loading.
Must work for **any** reasonable console or GUI .exe (MinGW- or MSVC-built), with
`todo.exe` (MinGW GUI app: ListView, Edit, ComboBox, Buttons, tray icon, popup menu,
registry, MessageBox) as the acceptance test.

## How relinking works (tool/anywin.c — done)
* The PE headers + sections are emitted at their original virtual addresses
  (`--section-start`), so no relocation processing is needed.
* Each IAT slot becomes `.quad __aw_<module>_<function>`, resolved at link time against
  `build/libanywin.o` (the runtime, one `ld -r` object). Module names are normalized by
  `runtime/modmap.h` (`KERNEL32.dll` -> `kernel32`, `api-ms-win-crt-*`/`ucrtbase`/`msvcr*` ->
  `msvcrt`, `api-ms-win-core-*` -> `kernel32`, ...). Data imports (e.g. `msvcrt!_acmdln`)
  bind the same way: the symbol is the variable.
* Imports the runtime doesn't define get a weak stub that prints
  `unimplemented function called: DLL!Name` and returns 0 (abort with `ANYWIN_STRICT=1`).
* Link: `cc -static -no-pie image.o libanywin.o -lm -lpthread`.
  Extra link flags: env `ANYWIN_LDFLAGS` (e.g. `-Wl,--unresolved-symbols=ignore-all` while
  other runtime parts are still missing). `anywin --list foo.exe` shows which imports exist.

## Runtime conventions (runtime/)
* Every exported Win32 function is `ms_abi` and defined with the macros in `aw.h`:
  ```c
  API(kernel32, DWORD, GetTickCount, (void)) { ... }
  APIDATA(msvcrt, char *, _acmdln) = NULL;
  API_ASM(msvcrt, "??2@YAPEAX_K@Z", void *, op_new, (size_t n)) { ... }  /* non-C names */
  ALIAS(msvcrt, _strdup, __aw_msvcrt_strdup)                               /* same TU only */
  ```
  This defines `__aw_kernel32_GetTickCount` and registers it in the `aw_exports` section,
  which backs `GetProcAddress`/`LoadLibrary` (`aw_find_export`).
* Call other exported functions directly by their symbol, e.g.
  `__aw_user32_SendMessageW(...)`; declare the prototype yourself (`WINAPI` = ms_abi).
* Variadic Win64 functions: define them `WINAPI` with `...` and use
  `__builtin_ms_va_list` / `__builtin_ms_va_start` / `__builtin_ms_va_arg`.
  A Win64 `va_list` passed in by the program (vprintf etc.) is a plain `char *` pointing at
  8-byte argument slots (doubles are stored as 8 bytes; anything > 8 bytes is passed by pointer).
* Callbacks into the program (WNDPROC, thread procs, qsort comparators, atexit, TLS
  callbacks...) must be called through `AWCALL` (ms_abi) function pointer types.
* Windows `long` is 32-bit, `wchar_t` is 16-bit (`WCHAR`). Never use glibc `wchar_t` functions
  on Windows strings. Never pass glibc `FILE*`/`struct stat`/`time_t*` layouts to the program.
* **ANSI code page is UTF-8 (65001).** `...A` functions take UTF-8; `aw_u2w`/`aw_w2u`
  convert (invalid UTF-8 bytes decode as Latin-1). Internally prefer UTF-8 or UTF-16 as fits.
* Paths: programs see Windows paths. `aw_unix_path()` maps `C:\x` -> `~/.anywin/drive_c/x`,
  `Z:\x` -> `/x`, `\`->`/`, case-insensitive component matching; `aw_win_path()` maps back
  (`/home/u` -> `Z:\home\u`). Environment defaults (APPDATA = `Z:\home\u\.config`, etc.) are
  set at startup by kernel32.
* Last error: `aw_set_last_error()` / `aw_get_last_error()` (stored in the TEB).
* TEB is reachable through `%gs` (set by `aw_teb_create`); PEB via `gs:[0x60]`.
* Kernel handles: `aw_handle_alloc/aw_handle_get/aw_handle_object` (sync.c). GDI handles,
  HWNDs and HMENUs are small table indices that fit in 32 bits (programs truncate them).
* Logging: `TRACE(...)` (enabled with `ANYWIN_DEBUG=1`), `aw_log`, `aw_fatal`.
* GUI layering and the cross-layer internal API are defined in `runtime/gui.h`.
  Built-in controls/dialogs/menu windows are **Unicode (W) window procs** (like Wine);
  user32 converts A<->W message parameters when calling procs across that boundary.

## Build & test
* `make BUILD=build-<you>` builds `build-<you>/anywin` and `build-<you>/libanywin.o`.
  Use your own BUILD dir: several people build concurrently. Files owned by others may be
  temporarily broken; build your own objects with `make -k BUILD=...` or compile single files.
* Test programs: C sources in `tests/` compiled with `x86_64-w64-mingw32-gcc` (also `-municode`,
  `-mwindows`, C++ with `x86_64-w64-mingw32-g++`), then relinked with
  `build-<you>/anywin tests/x.exe -o build-<you>/x` and run. GUI tests can run under
  `xvfb-run -a` (Xvfb, xdotool and ImageMagick `import` are installed; real display is `:0`).
* Gotchas: the output is static non-PIE; the PE image sits at its image base
  (usually 0x140000000), the runtime at 0x400000.

## File ownership
| Area | Files |
|---|---|
| relinker, startup, build | tool/anywin.c, runtime/start.c, runtime/modmap.h, Makefile, aw.h, gui.h |
| kernel32 | runtime/util.c, sync.c, kernel32*.c, file.c, console.c, locale.c, process.c |
| msvcrt / ucrt | runtime/msvcrt*.c, runtime/printf.c |
| SEH, advapi32, shell32, ole32, misc DLLs | runtime/seh.c, advapi32.c, shell32.c, ole32.c, misc_dlls.c |
| X11 platform + fonts | runtime/x11.c, runtime/font.c |
| gdi32 + regions + surfaces + user32 drawing helpers | runtime/gdi*.c, region.c, surface.c, drawtext.c |
| user32 core | runtime/user32*.c (windows, messages, input, painting, NC area, icons, clipboard) |
| menus, accelerators, dialogs, MessageBox | runtime/menu.c, runtime/dialog.c |
| controls + comctl32 | runtime/controls/*.c |
