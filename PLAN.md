# anywin — complete implementation plan (handoff)

This document has everything another agent harness needs to finish the project: the goal,
what has been learned so far, what already exists, the architecture and its fixed contracts,
and one self-contained work package per subagent, followed by the integration phase.

---

## 0. Goal

Build a **static Windows-to-Linux binary relinker** in C (Rust also allowed).

* Input: an x86-64 Windows PE executable.
* Output: a standalone native Linux ELF that runs by calling Linux implementations of the
  imported Win32 functions. **No Wine, no runtime compatibility layer, no PE loader at runtime.**
  Wine's design and ideas may be studied and reused, but nothing may depend on Wine at runtime.
* **Acceptance test:** `anywin todo.exe -o todo && ./todo` fully works on this machine
  (`/media/gringo/jogos/projetos/anywin/todo.exe`).
* **It must generalize** to arbitrary .exe files (console and GUI, MinGW- and MSVC-built), not
  just todo.exe. Don't special-case todo.exe anywhere.

Project root: `/media/gringo/jogos/projetos/anywin` (not a git repo yet; `git init` is fine).

---

## 1. Environment facts (verified)

* Arch Linux (CachyOS), kernel 6.18, x86-64 with `fsgsbase`; gcc 16.1, clang, `ld`, `nm`,
  static glibc (`/usr/lib/libc.a`) available. 12 cores.
* Cross tools for building test programs: `x86_64-w64-mingw32-gcc`, `x86_64-w64-mingw32-g++`,
  `x86_64-w64-mingw32-objdump`, `clang-cl`, `lld-link` (no MSVC CRT libs), `winedump` (analysis only).
* Desktop: X11 session (`DISPLAY=:0`, no Wayland), Cinnamon WM (has an XEmbed system tray),
  1920x1080, 96 DPI, `~/.Xauthority` with MIT-MAGIC-COOKIE-1.
* **No X11 development headers** (`/usr/include/X11/Xlib.h` absent; only `libX11.so` runtime)
  → the plan uses a hand-written X11 wire-protocol client (better anyway: fully static output).
* No `stb_truetype`/freetype headers → the plan includes a small TrueType rasterizer.
  Fonts present: `/usr/share/fonts/noto/NotoSans-{Regular,Bold,Italic,BoldItalic}.ttf`,
  `/usr/share/fonts/TTF/DejaVuSans*.ttf`, `DejaVuSansMono*.ttf`, `/usr/share/fonts/liberation/`.
* Testing tools: `Xvfb`, `xvfb-run`, `xdotool`, ImageMagick `import` (screenshots).
  **Never open test windows on the user's real display `:0`; use `xvfb-run -a`.**
* `rustc`/`cargo` exist but the shell prints a harmless `~/.cargo/env` warning on every command.

---

## 2. What todo.exe is (analysis results)

`file`: PE32+ x86-64, **GUI subsystem (2)**, MinGW-w64 (GCC 15.1, MSYS2), ImageBase
`0x140000000`, SectionAlignment 0x1000, SizeOfImage 0x27000, entry RVA 0x13f0, has
relocations, `.pdata`/`.xdata`, a TLS directory (RVA 0xd440, index slot, callbacks),
resources (`.rsrc`: one 32bpp DIB icon id 1, a manifest asking for Common-Controls v6 and
PerMonitorV2 DPI awareness). Sections: `.text .data .rdata .pdata .xdata .bss .idata .tls .rsrc .reloc`.

### Imports (complete list)
* **ADVAPI32**: RegCloseKey, RegDeleteValueA, RegOpenKeyExA, RegQueryValueExA, RegSetValueExA
* **GDI32**: CreateFontA, CreateSolidBrush, DeleteObject, GetStockObject, SetBkColor, SetTextColor
* **KERNEL32**: CreateDirectoryA, CreateMutexA, DeleteCriticalSection, EnterCriticalSection,
  GetLastError, GetModuleFileNameA, GetModuleHandleA, GetProcAddress, GetStartupInfoA,
  InitializeCriticalSection, IsDBCSLeadByteEx, LeaveCriticalSection, MulDiv,
  MultiByteToWideChar, SetUnhandledExceptionFilter, Sleep, TlsGetValue, VirtualProtect,
  VirtualQuery, WideCharToMultiByte
* **msvcrt**: __C_specific_handler, ___lc_codepage_func, ___mb_cur_max_func, __getmainargs,
  __initenv (data), __iob_func, __set_app_type, __setusermatherr, _acmdln (data), _amsg_exit,
  _cexit, _commode (data), _errno, _fmode (data), _initterm, _ismbblead, _localtime64, _strlwr,
  _time64, abort, atexit, calloc, exit, fclose, fopen, fprintf, fputc, fread, free, fwrite,
  getenv, localeconv, malloc, memcpy, signal, strcat, strcmp, strcpy, strerror, strftime,
  strlen, strncmp, strstr, vfprintf, wcslen
* **SHELL32**: Shell_NotifyIconA
* **USER32**: AppendMenuA, CreatePopupMenu, CreateWindowExA, DefWindowProcA, DestroyMenu,
  DispatchMessageA, EnableWindow, FindWindowA, GetClientRect, GetCursorPos, GetMessageA,
  GetWindowTextA, InvalidateRect, IsWindowVisible, LoadCursorA, LoadIconA, MessageBoxA,
  MoveWindow, PostMessageA, PostQuitMessage, RegisterClassA, SendMessageA, SetClassLongPtrA,
  SetFocus, SetForegroundWindow, SetWindowPos, SetWindowTextA, ShowWindow, TrackPopupMenu,
  TranslateMessage, UpdateWindow
* Runtime lookup: `GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow")`
  (must work or return NULL gracefully — implement it, returning 96).

### Behavior (from disassembly)
* Single instance via `CreateMutexA("SimpleTodoApp_Mutex")` + `FindWindowA`.
* Data file: `getenv("APPDATA")` + `\TodoApp` (`CreateDirectoryA`) + `\todos.dat` via
  `fopen`/`fread`/`fwrite` (paths use backslashes → must be translated).
* Main window class "TodoApp", title "Todo Application", 800x650.
* WndProc handles: WM_CREATE(1), WM_DESTROY(2), WM_SIZE(5), **WM_CLOSE(0x10) → ShowWindow(SW_HIDE)**
  (hides to tray!), WM_NOTIFY(0x4e), WM_COMMAND(0x111), WM_CTLCOLOREDIT/LISTBOX(0x133/0x134)
  and WM_CTLCOLORSTATIC(0x138) (SetTextColor 0x333333, SetBkColor white, returns
  GetStockObject(WHITE_BRUSH)), WM_DPICHANGED(0x2e0, uses the suggested RECT in lParam),
  WM_USER+1 (0x401, the tray callback).
* Controls created in WM_CREATE: `SysListView32` style 0x50800009 (WS_CHILD|WS_VISIBLE|WS_BORDER|
  LVS_REPORT|LVS_SHOWSELALWAYS), LVM_SETEXTENDEDLISTVIEWSTYLE 0x21 (FULLROWSELECT|GRIDLINES),
  6× LVM_INSERTCOLUMNA (Title, Description, Priority, Status, Created, …); "STATIC" labels
  ("Title:", "Description:", "Priority:"); "EDIT" fields (incl. a "Search todos..." edit);
  "COMBOBOX" with CB_ADDSTRING ×5 ("1".."5") + CB_SETCURSEL/CB_GETCURSEL; "BUTTON"s
  (Add/Edit/Save/Cancel/Delete/Complete, checkbox "Start with Windows"). Every control gets
  WM_SETFONT with a `CreateFontA("Segoe UI", -MulDiv(…dpi), …, CLEARTYPE_QUALITY)` font.
* ListView messages used: LVM_DELETEALLITEMS(0x1009), LVM_INSERTITEMA, LVM_SETITEMTEXTA(0x102e),
  LVM_GETITEMTEXTA(0x102d), LVM_GETNEXTITEM(0x100c, LVNI_SELECTED), LVM_GETITEMA(0x1005, LVIF_PARAM),
  LVM_SORTITEMS(0x1051) **with a program comparator callback (ms_abi!)**, LVM_SETEXTENDEDLISTVIEWSTYLE(0x1036).
* Notifications consumed: LVN_ITEMCHANGED (-101; checks uNewState & LVIS_SELECTED, iItem at
  NMLISTVIEW+0x18, uNewState +0x20), LVN_COLUMNCLICK (-108; iSubItem at +0x1c → sort),
  NM_DBLCLK (-3). Edit-control EN_CHANGE for the search box (via WM_COMMAND).
* Tray: `Shell_NotifyIconA` NIM_ADD with uCallbackMessage WM_USER+1; on right click builds a popup
  menu (`CreatePopupMenu`/`AppendMenuA`: Show/Hide/"Start with Windows"/Exit, ids 0x64..0x67),
  `GetCursorPos`, `SetForegroundWindow`, `TrackPopupMenu`, `PostMessageA(WM_NULL)`.
  Left/double click shows the window.
* "Start with Windows": `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value "TodoApp" =
  module path (RegOpenKeyExA/RegSetValueExA/RegQueryValueExA/RegDeleteValueA).
* MessageBoxA for validation errors ("Please enter a title!", …).
* MinGW CRT startup also exercises: TLS callbacks, `__getmainargs`, `_initterm`, pseudo-relocation
  code calling `VirtualQuery`/`VirtualProtect` on its own sections, `SetUnhandledExceptionFilter`,
  `__iob_func()+offset` as `stderr`, `GetStartupInfoA` for nShowCmd, TEB reads (`gs:[0x30]`,
  `gs:[0x8]` StackBase in `__native_startup_lock`), `TlsGetValue`.

---

## 3. Architecture (decided)

```
anywin (tool, C)                         libanywin.o (runtime, C, one `ld -r` object)
 parse PE ─┐                              start.c     main(): PEB/TEB via %gs, PE TLS, entry
 flat image│  image.S: .incbin chunks      kernel32    process/env/files/threads/sync/memory/locale
 IAT slots ├─► + `.quad __aw_<mod>_<fn>`   msvcrt      CRT incl. printf engine, FILE wrapping
 sections  │  per-section --section-start  seh         Windows x64 exception dispatch/unwinding
 weak stubs┘  cc -static -no-pie ─► ELF    advapi32    registry (files), shell32 (tray, folders)
                                           misc DLLs   ws2_32, winmm, ole32, comdlg32, shlwapi...
                                           x11.c       raw X11 protocol client (no libX11)
                                           font.c      TrueType rasterizer
                                           gdi*        DCs, objects, raster ops, text, regions
                                           user32*     windows, messages, painting, input, NC
                                           menu/dialog menus, accelerators, dialogs, MessageBox
                                           controls/   Button/Static/Edit/ListBox/ComboBox/
                                                       SysListView32/Header/... + comctl32
```

### 3.1 Relinking (tool/anywin.c — **done**, builds clean)
* Loads the PE, builds a flat image of SizeOfImage (headers + sections at their RVAs).
* Parses the import directory and the **delay-import** directory; every IAT slot becomes a
  link-time relocation `.quad __aw_<module>_<function>` (ordinal-only imports resolve via
  `aw_ordinal_name()`, else `__aw_<module>_#<ord>`).
* Emits `image.S` with one ELF section per PE section (`.aw_hdr`, `.aw_s0`, `.aw_s1`, …) using
  `.incbin` chunks; all-zero sections become `@nobits`; flags from PE characteristics.
  Each section is placed at its exact VA with `-Wl,--section-start=.aw_sN=0x…`.
  Because the image sits at its preferred base, **no base relocations are applied**.
* Emits weak definitions for every import symbol: `lea .LnameN(%rip),%rax; jmp __aw_unimplemented`
  → prints `unimplemented function called: DLL!Name` once and returns 0
  (abort with `ANYWIN_STRICT=1`). The runtime is a single relocatable object (not an archive),
  so its strong definitions always override these weak stubs.
* Emits `__aw_image_base`, `__aw_image_size` (absolute), `__aw_image_entry`, `__aw_image_subsystem`.
* Links: `cc -static -no-pie -o out image.o libanywin.o -Wl,-Ttext-segment=<0x400000 or
  0x7e0000000 if the PE overlaps> --section-start… -lm -lpthread` (+ `$ANYWIN_LDFLAGS`).
* Options: `-o`, `-r <runtime.o>` (default `libanywin.o` next to the tool), `--keep`, `--list`
  (shows each import as ok/MISS by checking `nm` of the runtime), `-v`.
* Possible later improvements: embed a fallback TTF into the output (`--embed-font`), PE images
  without relocations whose base collides with the runtime (already moves the runtime),
  PE section alignment < 0x1000 (currently rejected).

### 3.2 Process startup (runtime/start.c — **done**)
* `main()` → fills a static PEB (ImageBaseAddress at +0x10, ProcessParameters +0x20,
  OS version 10.0.19045 at +0x118..), creates the main TEB (`aw_teb_create`: 0x2000 bytes,
  ExceptionList=-1, StackBase/StackLimit from `pthread_getattr_np`, Self at +0x30, ClientId,
  ThreadLocalStoragePointer at +0x58 with the PE TLS block in slot `tls_index`, PEB at +0x60,
  LastError at +0x68, TlsSlots at +0x1480, TlsExpansionSlots at +0x1780) and sets `%gs` with
  `arch_prctl(ARCH_SET_GS)`. glibc uses `%fs`, so `%gs` is free and per-thread.
* Then: `aw_kernel32_init(argc, argv, envp)`, `aw_msvcrt_init()`, `aw_seh_init()`, installs
  SIGSEGV/SIGBUS/SIGILL/SIGFPE handlers (`aw_seh_signal()` first, else a crash dump with RIP/RVA),
  runs PE TLS callbacks (DLL_PROCESS_ATTACH), calls the PE entry point as ms_abi with the PEB,
  then `ExitProcess(ret)`.
* `aw_thread_attach()/aw_thread_detach()` run TLS callbacks for new threads.
* Exports registry: `aw_find_export(module, name)` scans the `aw_exports` section
  (`__start_aw_exports..__stop_aw_exports`) — backs GetProcAddress/LoadLibrary.

### 3.3 Fixed contracts (read these files; do not change signatures without coordinating)
* `runtime/aw.h` — Win32 base types, `WINAPI`/`AWCALL` (= `ms_abi`), callback typedefs, and the
  export macros:
  ```c
  API(kernel32, DWORD, GetTickCount, (void)) { ... }          // defines __aw_kernel32_GetTickCount
  APIDATA(msvcrt, char *, _acmdln) = NULL;                    // data import
  API_ASM(msvcrt, "??2@YAPEAX_K@Z", void *, op_new, (size_t)) // non-C symbol names
  ALIAS(msvcrt, _strdup, __aw_msvcrt_strdup)                  // same translation unit only
  ```
  plus runtime helpers: `aw_set_last_error/aw_get_last_error`, `aw_u2w/aw_w2u(_buf)`,
  `aw_unix_path/aw_unix_pathW/aw_win_path`, `aw_errno_to_win`, handle table
  (`aw_handle_alloc/get/object/fd`, types `H_FILE, H_EVENT, H_MUTEX, H_SEM, H_THREAD,
  H_PROCESS, H_FIND, H_MAPPING, H_CONSOLE_IN`), `aw_now_filetime`, `aw_tick_ms`,
  `aw_find_resource(module, type, name, &size)`, `aw_gui_atexit`, `TRACE/aw_log/aw_fatal`.
* `runtime/modmap.h` — DLL name normalization shared by tool and runtime
  (`api-ms-win-crt-*`, `ucrtbase`, `msvcr*`, `vcruntime*` → `msvcrt`; `api-ms-win-core-registry*`,
  `api-ms-win-security*`, `sechost` → `advapi32`; other `api-ms-win-core-*`, `kernelbase` →
  `kernel32`; `wsock32` → `ws2_32`; …) and the ordinal→name table (comctl32, ws2_32, oleaut32).
* `runtime/gui.h` — the cross-layer GUI API (surfaces, regions, `plat_*` X11 layer, `font_*`,
  GDI internals for user32, `aw_icon`, init hooks, menubar hooks, `aw_modal_loop`, `aw_syscolor`).
  Summarized in §5 below; the header is authoritative.
* `docs/ARCHITECTURE.md` — conventions (also summarized in §4).

### 3.4 Existing files and status
| File | Status |
|---|---|
| `tool/anywin.c` | done (552 lines) |
| `runtime/aw.h`, `gui.h`, `modmap.h` | done (contracts) |
| `runtime/start.c` | done |
| `runtime/util.c` | done: UTF-8⇄UTF-16, Win⇄Unix paths (drive mapping, case-insensitive lookup, devices NUL/CON/CONIN$/CONOUT$), errno→Win32 error |
| `runtime/sync.c` | done (~80 APIs): handle table, CloseHandle/DuplicateHandle, CreateThread/ExitThread/Resume/GetExitCodeThread, events/mutexes/semaphores (named), WaitForSingle/MultipleObjects(Ex), SignalObjectAndWait, critical sections (futex), SRW locks, condition variables, WaitOnAddress, InitOnce, TlsAlloc/Get/Set/Free, FlsAlloc/…, SList. Exposes accessors `aw_obj_*`, `aw_create_thread`, `aw_wait_multiple`, `aw_wait_lock/unlock`, `aw_obj_set_finished`. |
| `Makefile` | done: `make BUILD=dir` → `dir/anywin`, `dir/libanywin.o` (all `runtime/*.c` + `runtime/controls/*.c`) |
| everything else | **to do** (work packages below) |

All three existing runtime .c files compile warning-free. Nothing links yet (missing
`aw_kernel32_init`, `aw_msvcrt_init`, `aw_seh_init`, `aw_seh_signal`, `aw_file_close`,
`aw_process_poll`, `aw_fls_thread_exit` is in sync.c, etc.).

---

## 4. Conventions every subagent must follow

1. Every exported Win32 function is `ms_abi`, defined with `API(...)`. Call other exports
   directly (`__aw_user32_SendMessageW(...)`), declaring the prototype yourself.
2. Variadic Win64 functions: `WINAPI` + `...` + `__builtin_ms_va_list/__builtin_ms_va_start/
   __builtin_ms_va_arg`. A `va_list` received from the program is a `char *` to 8-byte slots
   (doubles occupy 8 bytes; args > 8 bytes are passed by pointer).
3. Calls into program code (WNDPROC, DLGPROC, thread procs, comparators, atexit/onexit,
   TLS callbacks, timer procs, enum callbacks, FLS callbacks) go through `AWCALL` pointer types.
4. Windows `long` = 32-bit, `wchar_t` = 16-bit `WCHAR`. Never use glibc `wchar_t` functions on
   Windows strings; never expose glibc struct layouts (FILE, stat, tm, lconv, addrinfo, fd_set…).
5. **ANSI code page = UTF-8 (65001).** `…A` APIs take UTF-8; invalid UTF-8 bytes decode as Latin-1.
6. Programs see Windows paths: `C:\x` → `~/.anywin/drive_c/x` (override `ANYWIN_DRIVE_C`),
   `Z:\x` → `/x`; backslashes → slashes; case-insensitive component fallback. Paths handed to the
   program are Windows style (`Z:\home\user\…`).
7. Kernel handles are multiples of 4 from the table in sync.c. GDI handles, HWNDs, HMENUs are
   small table values that fit in 32 bits (programs truncate them). HICON/HCURSOR are pointers
   to `aw_icon` (fine — they are 64-bit on Windows too).
8. Last error lives in the TEB; set it where Windows does and programs check it.
9. Built-in window classes (controls, dialogs, menus, message boxes) are **Unicode (W)** window
   procs, like Wine; user32 converts A⇄W message parameters across the boundary.
10. Logging: `TRACE()` only (enabled by `ANYWIN_DEBUG=1`). No noisy stderr output otherwise.
11. Thread safety where Windows guarantees it (kernel objects, heap, CRT); GUI is per-thread but
    must not crash if used from a second thread.
12. Each subagent builds in its own dir: `make -k BUILD=build-<name>` (others' files may be
    temporarily broken). Its own files must compile **warning-free** with the Makefile flags.
13. To test before the whole runtime links: `ANYWIN_LDFLAGS=-Wl,--unresolved-symbols=ignore-all`.
    Prefer also native Linux unit-test harnesses that link only your own objects.
14. Test programs live in `tests/<area>_*.c`, built with mingw (`x86_64-w64-mingw32-gcc`,
    `-municode`, `-mwindows`, `-nostdlib -e entry` for kernel32-only tests, g++ for C++), relinked
    with `build-<name>/anywin tests/x.exe -o build-<name>/x`, GUI ones run under `xvfb-run -a`.
15. Don't edit files you don't own. If a shared header truly needs something, append a small,
    clearly marked block (or describe the needed change in your report).

---

## 5. The GUI contract (summary of runtime/gui.h)

* **Surfaces** (`aw_surface`): 32bpp `0x00RRGGBB`, `px` = top row, `stride` in pixels (may be
  negative for bottom-up DIBs), `dirty`/`has_dirty` = area modified since the owner last flushed.
  `surface_new/wrap/free/resize/mark_dirty`, `surface_row()`. Implemented by GDI.
* **Regions** (`aw_region`): non-overlapping rect lists; `rgn_init/free/clear/copy/set_rect/
  union_rect/subtract_rect/intersect_rect/union/subtract/intersect/offset/is_empty/bbox/
  contains_point/intersects_rect`. Implemented by GDI; used by user32 for update/visible regions.
* **Platform** (`plat_*`, x11.c): `plat_init` (idempotent, returns 0 without display), `plat_fd`,
  `plat_window_create(x,y,w,h, kind, owner)` with kinds NORMAL (managed), DIALOG (managed,
  transient), POPUP (override-redirect), destroy/show/move/set_title/set_icon/set_resizable/
  activate/minimize/maximize/get_pos, `plat_put(win, surface, rect)`, `plat_flush`,
  `plat_poll(&ev, timeout_ms)` (returns 0 on timeout or `plat_wakeup()`), `plat_cursor_pos`,
  `plat_screen_size`, `plat_set_cursor(win, IDC_*)`, `plat_grab`, clipboard get/set (UTF-8),
  tray `plat_tray_add/remove/draw`, `plat_bell`. Events (`aw_plat_event`): EXPOSE, CONFIGURE,
  CLOSE, KEYDOWN/UP (with Windows `vk`, set-1 `scancode`, `is_repeat`, UTF-8 `text`),
  BUTTONDOWN/UP (1 L, 2 M, 3 R, 4 X1, 5 X2), MOTION, WHEEL (±120, horizontal flag), ENTER/LEAVE,
  FOCUSIN/OUT, MAP/UNMAP; `mods` = AW_MOD_* (shift/ctrl/alt/buttons/locks).
* **Fonts** (`font_*`, font.c): `font_open(face, lfHeight, width, weight, italic, underline,
  strikeout)` (LOGFONT height semantics; cached; never NULL), `font_metrics` (ascent, descent,
  height, internal/external leading, ave/max char width, …), `font_glyph(f, codepoint)`
  (AA coverage bitmap + left/top/advance, never NULL), `font_advance`.
* **GDI internals**: `gdi_create_window_dc(hwnd, surf, org_x, org_y, vis)` (logical 0,0 = surface
  org; `vis` = visible region in surface coords, copied; drawing never leaves it and marks
  `surf->dirty`), `gdi_is_window_dc`, `gdi_delete_window_dc`, `gdi_dc_window`, `gdi_dc_font`,
  `gdi_draw_argb(hdc, x,y,w,h, argb, sw, sh)` (alpha-blended scaled icon drawing),
  `gdi_bitmap_surface(hbmp)`.
* **user32 internals**: `aw_icon` struct (magic, w, h, argb, hotspot, is_cursor, idc, shared),
  `aw_icon_from_handle`, `aw_user_init()` (lazy: plat_init + registers built-in classes by
  calling `aw_controls_register()`, `aw_dialogs_register()`, `aw_menus_register()`),
  menubar hooks implemented by menu.c (`aw_menubar_height/paint/track`, `aw_menu_is_valid`),
  `aw_modal_loop(&done)`, `aw_syscolor(idx)`.
* **Top-level window model:** a Win32 top-level window maps to one X window whose X *client area
  equals the Win32 window rect* (the X window manager draws the real caption/frame; Win32
  NC area for top-levels is only the optional menu bar). `AdjustWindowRect(Ex)` therefore adds
  only the menu bar height. Child windows are not X windows: they are drawn into their top-level's
  backbuffer surface with clipping (children always clipped out of parents, siblings by z-order).
  Kind rule: WS_POPUP without WS_CAPTION → POPUP (override-redirect); owned WS_POPUP with caption →
  DIALOG; else NORMAL.

---

## 6. Work packages (one subagent each; all can run in parallel)

Each package's brief = §0–§5 of this file + its section below. Every agent reports back:
files written, design decisions, skipped functions, interface assumptions others need to know,
and test results.

### A. kernel32 — owns `util.c`, `sync.c` (existing, keep its internal API; fix bugs), new `kernel32.c`, `file.c`, `console.c`, `locale.c`, `process.c`, optional `k32.h`

Required hooks:
* `aw_kernel32_init(argc, argv, envp)`: build the Windows command line (argv[0] → Windows path of
  `/proc/self/exe`, quoted; args quoted per MS rules); set environment defaults if absent:
  APPDATA (= Windows path of `$XDG_CONFIG_HOME` or `~/.config`), LOCALAPPDATA (`~/.local/share`),
  USERPROFILE, HOMEDRIVE/HOMEPATH, TEMP/TMP (`Z:\tmp`), USERNAME, COMPUTERNAME, SystemRoot/windir
  (`C:\windows`), SystemDrive, ProgramFiles, ProgramData, ALLUSERSPROFILE, PUBLIC, OS=Windows_NT,
  PROCESSOR_ARCHITECTURE=AMD64, NUMBER_OF_PROCESSORS, PATHEXT, ComSpec. Create std handles
  (H_FILE objects on fd 0/1/2, console flag if isatty).
* `aw_file_close(aw_object*)` (CloseHandle for H_FILE/H_FIND/H_MAPPING), `aw_process_poll(aw_object*)`
  (child reaping for waits), `aw_find_resource()` (parse the image's `.rsrc`; module NULL or image
  base; type/name = MAKEINTRESOURCE ints or UTF-16 strings, case-insensitive; `"#123"` = id 123;
  first language), `aw_now_filetime()`, `aw_tick_ms()`.
* Pseudo modules: GetModuleHandle/LoadLibrary succeed for any DLL whose normalized namespace has
  at least one export in `aw_exports` (declare `extern const struct aw_export __start_aw_exports[],
  __stop_aw_exports[]`), returning a stable fake handle that points to a minimal fake PE header
  (MZ, PE signature, optional header, zero data directories); unknown DLLs fail with 126.
  GetModuleHandle(NULL)/own name → `__aw_image_base`. GetProcAddress on fake modules → export
  table (ordinals via `aw_ordinal_name`); on the image → its own export directory.
  GetModuleFileName(NULL) → Windows path of the ELF; fake modules → `C:\windows\system32\<n>.dll`.
* ExitProcess/TerminateProcess(self): call `aw_gui_atexit` if set, `fflush(NULL)`, `_exit(code)`.
* NOT yours: SetUnhandledExceptionFilter, UnhandledExceptionFilter, RaiseException, Rtl*Unwind*,
  RtlCaptureContext, RtlLookupFunctionEntry (→ package C); anything msvcrt (→ B).

Implement (A and W): process/module/env (ExitProcess, TerminateProcess, GetCurrentProcess(Id),
GetModuleHandleA/W/ExA/ExW, GetModuleFileNameA/W, GetProcAddress, LoadLibraryA/W/ExA/ExW,
FreeLibrary, DisableThreadLibraryCalls, GetCommandLineA/W, GetStartupInfoA/W (no
STARTF_USESHOWWINDOW, wShowWindow SW_SHOWDEFAULT), Get/SetEnvironmentVariableA/W (case-insensitive),
GetEnvironmentStrings(A)/W, FreeEnvironmentStringsA/W, ExpandEnvironmentStringsA/W, GetVersion,
GetVersionExA/W (10.0.19045), VerifyVersionInfoA/W, VerSetConditionMask, IsProcessorFeaturePresent,
GetSystemInfo, GetNativeSystemInfo, IsWow64Process, IsDebuggerPresent, CheckRemoteDebuggerPresent,
OutputDebugStringA/W, DebugBreak, Set/GetErrorMode, Get/SetLastError, FormatMessageA/W
(FROM_SYSTEM with a table of common messages, ALLOCATE_BUFFER, IGNORE_INSERTS, FROM_STRING with
%1..%n from va_list or array), GetComputerNameA/W(Ex), GetSystemDirectoryA/W, GetWindowsDirectoryA/W,
GetSystemWindowsDirectoryA/W, GetTempPathA/W(2), GetTempFileNameA/W, Get/SetCurrentDirectoryA/W,
GetFullPathNameA/W, GetLongPathNameA/W, GetShortPathNameA/W, Get/SetPriorityClass,
GetProcessTimes, GetSystemTimes, GetProcessAffinityMask, GetActiveProcessorCount,
GetLogicalProcessorInformation(Ex), GlobalMemoryStatus(Ex), K32GetProcessMemoryInfo,
ProcessIdToSessionId, WTSGetActiveConsoleSessionId, CreateProcessA/W (fork/exec; a Windows .exe
target with a relinked ELF next to it (same name without .exe) runs that ELF; `cmd.exe /c …` →
`/bin/sh -c`; STARTF_USESTDHANDLES with pipes), GetExitCodeProcess, OpenProcess (self), CreatePipe,
PeekNamedPipe, Sleep, SleepEx).
Memory: VirtualAlloc(Ex) (reserve = mmap PROT_NONE, commit = mprotect; honor lpAddress),
VirtualFree, VirtualProtect (mprotect; works on PE image pages; returns old protection),
VirtualQuery(Ex) (from `/proc/self/maps`; MinGW CRT queries its own sections), FlushInstructionCache,
GetProcessHeap, HeapCreate/Destroy/Alloc/ReAlloc/Free/Size/Validate/Compact/Lock/Unlock/
SetInformation/QueryInformation/Walk, GetProcessHeaps, GlobalAlloc/Free/Lock/Unlock/Size/ReAlloc/
Handle/Flags, LocalAlloc/Free/Lock/Unlock/Size/ReAlloc, RtlMoveMemory/RtlZeroMemory/RtlFillMemory/
RtlCopyMemory, IsBad*Ptr (FALSE), CreateFileMappingA/W, OpenFileMappingA/W, MapViewOfFile(Ex),
UnmapViewOfFile, FlushViewOfFile. HeapSize returns the exact requested size; HEAP_ZERO_MEMORY honored.
Time: GetTickCount(64), QueryPerformanceCounter/Frequency (10 MHz), GetSystemTimeAsFileTime,
GetSystemTimePreciseAsFileTime, GetSystemTime, GetLocalTime, SystemTimeToFileTime,
FileTimeToSystemTime, FileTimeToLocalFileTime, LocalFileTimeToFileTime,
SystemTimeToTzSpecificLocalTime, TzSpecificLocalTimeToSystemTime, GetTimeZoneInformation,
GetDynamicTimeZoneInformation, CompareFileTime, FileTimeToDosDateTime, DosDateTimeToFileTime.
Files: CreateFileA/W (all dispositions; OPEN_ALWAYS/CREATE_ALWAYS on existing → succeed + error 183;
DELETE_ON_CLOSE, APPEND_DATA, directories with BACKUP_SEMANTICS), ReadFile, WriteFile (OVERLAPPED
offsets honored synchronously), ReadFileEx/WriteFileEx, GetOverlappedResult, SetFilePointer(Ex),
GetFileSize(Ex), GetFileType, FlushFileBuffers, SetEndOfFile, DeleteFileA/W, MoveFile(Ex)A/W,
CopyFile(Ex)A/W, ReplaceFileA/W, CreateDirectory(Ex)A/W, RemoveDirectoryA/W, Get/SetFileAttributesA/W,
GetFileAttributesExA/W, FindFirstFile(Ex)A/W, FindNextFileA/W, FindClose (Windows wildcards; `*.*`
matches everything), Get/SetFileTime, GetFileInformationByHandle(Ex), GetFinalPathNameByHandleA/W,
GetDiskFreeSpace(Ex)A/W, GetDriveTypeA/W, GetLogicalDrives, GetLogicalDriveStringsA/W,
GetVolumeInformationA/W, Lock/UnlockFile(Ex), SearchPathA/W, GetBinaryTypeA/W, CreateHardLinkA/W,
CreateSymbolicLinkA/W, Get/SetStdHandle, DeviceIoControl (fail), CancelIo, Get/SetHandleInformation;
INI: GetPrivateProfileString/Int/Section/SectionNamesA/W, WritePrivateProfileString/SectionA/W,
Get/WriteProfileStringA/W (relative INI names live in the Windows directory).
Console: WriteConsoleA/W, ReadConsoleA/W, Get/SetConsoleMode, Get/SetConsole(Output)CP (65001),
Set/GetConsoleTitleA/W, GetConsoleScreenBufferInfo(Ex) (TIOCGWINSZ), SetConsoleTextAttribute (ANSI
SGR on ttys), SetConsoleCursorPosition, FillConsoleOutputCharacterA/W, FillConsoleOutputAttribute,
ScrollConsoleScreenBuffer, Get/SetConsoleCursorInfo, Alloc/Free/AttachConsole, GetConsoleWindow
(NULL), SetConsoleCtrlHandler (SIGINT → handlers with CTRL_C_EVENT on a new thread),
GenerateConsoleCtrlEvent, GetNumberOfConsoleInputEvents, Peek/ReadConsoleInputA/W (raw tty key
events when ENABLE_LINE_INPUT is off), FlushConsoleInputBuffer, WriteConsoleOutputA/W,
GetLargestConsoleWindowSize, SetConsoleScreenBufferSize, SetConsoleWindowInfo.
Locale/strings: MultiByteToWideChar/WideCharToMultiByte (ACP/OEMCP/THREAD_ACP/UTF8 → UTF-8; real
tables for 1252, 28591, 437; MB_ERR_INVALID_CHARS; size queries; default char), GetACP (65001),
GetOEMCP (437), GetCPInfo(ExA/W), IsValidCodePage, IsDBCSLeadByte(Ex) (FALSE), lstrlen/cpy/cpyn/
cat/cmp/cmpiA/W, CompareStringA/W/Ex/Ordinal, LCMapStringA/W/Ex, FoldStringW, GetStringTypeA/W/ExA/ExW,
GetLocaleInfoA/W/Ex (en-US values for the common LOCALE_* ids), GetUserDefaultLCID/LangID/
UILanguage/LocaleName + System equivalents, Get/SetThreadLocale, IsValidLocale(Name),
LCIDToLocaleName, LocaleNameToLCID, GetNumberFormat(Ex)A/W, GetCurrencyFormatA/W,
GetDateFormat(Ex)A/W, GetTimeFormat(Ex)A/W, EnumSystemLocalesA/W, GetUserPreferredUILanguages,
ResolveLocaleName, GetNLSVersionEx, MulDiv (Windows rounding; -1 on /0).
Resources: FindResource(Ex)A/W, LoadResource, LockResource, SizeofResource, FreeResource,
EnumResourceNamesA/W.
Misc: Encode/DecodePointer, Encode/DecodeSystemPointer, Beep, QueryUnbiasedInterruptTime,
GetCurrentPackageId (15700), AreFileApisANSI, SetFileApisToANSI/OEM, Interlocked* exports,
GetNumaHighestNodeNumber, SetProcessDEPPolicy, SetProcessWorkingSetSize, GetProcessVersion,
GetThreadTimes, QueueUserAPC, CreateTimerQueue(+Timer)/DeleteTimerQueueTimer/DeleteTimerQueueEx,
CreateWaitableTimerA/W/SetWaitableTimer/CancelWaitableTimer, RegisterWaitForSingleObject/
UnregisterWait(Ex), QueueUserWorkItem, TrySubmitThreadpoolCallback, Create/Submit/WaitFor/Close
ThreadpoolWork, GetCurrentProcessorNumberEx, SetThreadStackGuarantee, SetThreadIdealProcessor,
GetSystemWow64DirectoryA/W (fail), Wow64Disable/RevertWow64FsRedirection.

Tests: kernel32-only programs (`-nostdlib -e entry -lkernel32`), covering files (incl.
case-insensitive lookup), env, command line, threads + all sync objects, TLS, heap, virtual memory,
time conversions, code pages, FormatMessage, INI, resources.

### B. msvcrt / ucrt — owns `msvcrt*.c`, `printf.c`

Required hook: `aw_msvcrt_init()` (called by start.c after kernel32 init, before the entry point):
initialize `_acmdln`/`_wcmdln` from GetCommandLineA/W, argc/argv/wargv (MS parsing rules),
`_environ`/`_wenviron`/`__initenv`/`__winitenv`, the iob array, locale.

Design:
* `FILE` = msvcrt layout (48 bytes: `_ptr,_cnt,_base,_flag,_file,_charbuf,_bufsiz,_tmpfname`);
  wrap a glibc `FILE*` (stored in a side field / `_tmpfname` slot; `_file` = fd). `_iob[3]` is a
  contiguous array (msvcrt data export `_iob`, `__iob_func()`, ucrt `__acrt_iob_func(i)`).
  stdio buffering via glibc so `fflush(NULL)` in ExitProcess flushes everything.
* fds are Linux fds directly; `_get_osfhandle` wraps an fd in an H_FILE kernel handle (use
  kernel32's `aw_handle_alloc` + `aw_obj_set_fd` accessors from sync.c), `_open_osfhandle` unwraps.
* Text/binary mode: treat as binary (no CRLF translation) but track the mode flags for `_setmode`.
* A single printf engine with Windows semantics, taking a Win64 arg-slot pointer: flags, width,
  precision, `*`, `h hh l(32-bit!) ll L I I32 I64 z t j w`, `%d i u o x X c C s S Z p n e E f F g G
  a A %`, `%s` in wide functions = wide, `%S` = opposite width; msvcrt-style `%e` 3-digit exponent
  when the image imports `msvcrt.dll`, 2 digits for ucrt; inf/nan spelling per CRT; floats via
  glibc `snprintf` per conversion. Wide output = UTF-16.
* scanf family: convert the format to glibc semantics (`%ld`→32-bit, `%I64d`→`%lld`, `%S`/`%ls`
  handling…) and pass up to 32 pointer args collected from the Win64 arg slots.

Functions (msvcrt names; ucrt names map here too):
startup/exit: __getmainargs, __wgetmainargs, _initterm, _initterm_e, __set_app_type, _set_app_type,
__setusermatherr, _amsg_exit, exit, _exit, _Exit, quick_exit, _cexit, _c_exit, abort, atexit,
_onexit, __dllonexit, _crt_atexit, _crt_at_quick_exit, _register_onexit_function,
_initialize_onexit_table, _execute_onexit_table, _configure_narrow_argv, _configure_wide_argv,
_initialize_narrow_environment, _initialize_wide_environment, _get_initial_narrow_environment,
_get_initial_wide_environment, __p___argc, __p___argv, __p___wargv, __p__environ, __p__wenviron,
__p__acmdln, __p__wcmdln, __p__fmode, __p__commode, __p___initenv, _set_fmode, _get_fmode,
_seh_filter_exe, _register_thread_local_exe_atexit_callback, _set_new_mode, _query_new_mode,
_setusermatherr, __lconv_init, _set_invalid_parameter_handler, _set_purecall_handler,
_invalid_parameter_noinfo(_noreturn), _get_pgmptr, _get_wpgmptr, _pgmptr, _wpgmptr, __argc,
__argv, __wargv, _acmdln, _wcmdln, _environ, _wenviron, __initenv, __winitenv, _fmode, _commode
(data). exit() runs atexit/onexit handlers (ms_abi, reverse order), flushes, calls ExitProcess.
locale/ctype: ___lc_codepage_func (65001), ___mb_cur_max_func (UTF-8 → 1 is what MinGW expects for
"C" locale; return 1 unless setlocale(".UTF8") was called), ___lc_handle_func, __pctype_func,
_pctype, __mb_cur_max, _isctype, all is*/to* for char and wide (`iswalpha` etc. on WCHAR),
_isleadbyte_l, _ismbblead, _ismbbtrail, _mbsinc, _mbsdec, _mbslen, _mbschr, _mbsrchr, _mbsstr,
_mbscmp, _mbsicmp, _mbsnbcpy, setlocale/_wsetlocale ("C" and "" accepted), localeconv (Windows
`struct lconv` layout), _create_locale/_free_locale/_get_current_locale + `_l` variants as aliases.
string/memory: mem*, str* (incl. _stricmp, _strnicmp, _strdup, _strlwr, _strupr, _strrev, strtok,
strtok_s, _strset, strspn, strcspn, strpbrk, strcoll, strxfrm, _stricoll), wcs* on WCHAR (wcslen,
wcscpy, wcsncpy, wcscat, wcscmp, wcsncmp, _wcsicmp, _wcsnicmp, wcschr, wcsrchr, wcsstr, wcstok,
_wcsdup, _wcslwr, _wcsupr, wcsspn, wcscspn, wcspbrk, wcscoll), secure variants (strcpy_s, strncpy_s,
strcat_s, strncat_s, wcscpy_s, wcsncpy_s, wcscat_s, memcpy_s, memmove_s, sprintf_s, swprintf_s,
_snprintf_s, vsprintf_s, _vsnprintf_s, _itoa_s, _ltoa_s, _ultoa_s, _i64toa_s, _wcslwr_s, _strlwr_s,
_strupr_s, strerror_s, _wcserror_s, getenv_s, _dupenv_s, _wdupenv_s, fopen_s, _wfopen_s,
localtime_s, gmtime_s, _localtime64_s, _gmtime64_s, asctime_s, ctime_s, qsort_s, mbstowcs_s,
wcstombs_s, _splitpath_s, _makepath_s, tmpnam_s).
stdlib: malloc/calloc/realloc/free/_msize/_expand/_recalloc, _aligned_malloc/_aligned_free/
_aligned_realloc/_aligned_offset_malloc, qsort/bsearch/_lfind/_lsearch (comparators are ms_abi!),
atoi/atol/_atoi64/atof/strtol/strtoul/strtoll/strtoull/_strtoi64/_strtoui64/strtod/strtof/strtold,
wcstol/wcstoul/wcstod/_wtoi/_wtol/_wtof/_wtoi64, _itoa/_ltoa/_ultoa/_i64toa/_ui64toa and wide
versions, _ecvt/_fcvt/_gcvt, abs/labs/llabs/_abs64/div/ldiv/lldiv, rand/srand/rand_s,
getenv/_wgetenv/_putenv/_wputenv/_putenv_s, system/_wsystem (sh -c), _sleep, _beep,
_splitpath/_wsplitpath, _makepath/_wmakepath, _fullpath/_wfullpath, _searchenv, mbstowcs/wcstombs/
mbtowc/wctomb/mblen/btowc/wctob/mbrtowc/wcrtomb (UTF-8 ⇄ UTF-16), _byteswap_*, _rotl/_rotr,
_lrotl/_lrotr, __crt_debugger_hook.
stdio: fopen/_wfopen/_fsopen/_wfsopen/freopen/_wfreopen/fclose/_fcloseall/fflush/_flushall/fread/
fwrite/fgetc/getc/_fgetchar/fputc/putc/_fputchar/fgets/fputs/_fgetws/fputws/fgetwc/fputwc/getwc/
putwc/ungetc/ungetwc/gets/puts/_putws/getchar/putchar/_getwchar/_putwchar/fseek/_fseeki64/ftell/
_ftelli64/rewind/fgetpos/fsetpos/feof/ferror/clearerr/setvbuf/setbuf/tmpfile/tmpnam/_tempnam/
_wtempnam/remove/_wremove/rename/_wrename/_unlink/_wunlink/_fileno/_fdopen/_wfdopen/_popen/_wpopen/
_pclose/_filbuf/_flsbuf/_lock_file/_unlock_file/_getmaxstdio/_setmaxstdio, all printf variants
(printf, fprintf, sprintf, snprintf, _snprintf, _scprintf, vprintf, vfprintf, vsprintf, vsnprintf,
_vsnprintf, _vscprintf, wprintf, fwprintf, swprintf (msvcrt's is `(buf, fmt, ...)`), _swprintf,
_snwprintf, vwprintf, vfwprintf, vswprintf, _vsnwprintf, _vscwprintf, _scwprintf, `_l` variants,
`_p` positional variants optional, ucrt `__stdio_common_vfprintf`, `__stdio_common_vsprintf`,
`__stdio_common_vsnprintf_s`, `__stdio_common_vsprintf_s`, `__stdio_common_vfwprintf`,
`__stdio_common_vswprintf`, `__stdio_common_vswprintf_s`, `__stdio_common_vsscanf`,
`__stdio_common_vswscanf`, `__stdio_common_vfscanf`), scanf family (scanf, fscanf, sscanf, wscanf,
fwscanf, swscanf, vscanf, vsscanf, vfscanf, `_s` variants).
io/fs: _open/_wopen/_sopen/_wsopen/_sopen_s/_creat/_close/_read/_write/_lseek/_lseeki64/_tell/
_telli64/_eof/_chsize/_chsize_s/_commit/_dup/_dup2/_pipe/_isatty/_setmode/_get_osfhandle/
_open_osfhandle/_access/_waccess/_access_s/_chmod/_wchmod/_mkdir/_wmkdir/_rmdir/_wrmdir/_chdir/
_wchdir/_getcwd/_wgetcwd/_getdcwd/_getdrive/_chdrive, _stat/_stat32/_stat64/_stat64i32/_stati64/
_wstat* and _fstat* variants (Windows struct layouts, st_mode bits _S_IFDIR etc.),
_findfirst/_findnext/_findclose (+ 32/64/i64 and wide variants; Windows `_finddata_t` layouts),
_utime/_wutime/_futime (+64), _umask, _locking, _getpid, _execv*/_spawnv* (via fork/exec).
time: time/_time32/_time64, _localtime32/64, localtime, _gmtime32/64, gmtime, _mktime32/64,
mktime, _mkgmtime(64), difftime(64), strftime/wcsftime (Windows `struct tm` = same field order as
glibc's first 9 ints), asctime/_wasctime, ctime/_ctime64/_wctime, clock (CLOCKS_PER_SEC 1000),
_ftime/_ftime64/_ftime_s, _tzset, _timezone/_daylight/_tzname (+ `__p__` variants), _strdate,
_strtime, _getsystime, timespec_get, _utime.
math: everything in math.h for double/float (`sinf` etc.), _hypot, _hypotf, _finite, _finitef,
_isnan, _isnanf, _copysign, _chgsign, _logb, _nextafter, _scalb, _fpclass, _j0/_j1/_jn/_y0/_y1/_yn,
_cabs, frexp, ldexp, modf/modff, fmod/fmodf, round/lround/llround/trunc/rint/lrint/nearbyint,
fma, cbrt, exp2, log2, log1p, expm1, erf/erfc/tgamma/lgamma, remainder, remquo, nan, fmin/fmax,
_controlfp/_controlfp_s/_control87/_statusfp/_clearfp/_fpreset/__fpe_flt_rounds/fegetenv/
fesetenv/feclearexcept/fetestexcept/fegetround/fesetround, _dclass/_fdclass/_dsign/_dtest
(ucrt), __setusermatherr, _matherr.
errno/signals/threads: _errno (per-thread int*; Windows errno numbering: values ≤ 34 match Linux,
remap the rest, e.g. EDEADLK 36, ENAMETOOLONG 38, ENOTEMPTY 41, EILSEQ 42), __doserrno,
_get_errno/_set_errno, _get_doserrno, strerror/_strerror/_wcserror (Windows messages), perror/
_wperror, signal/raise (SIGINT 2, SIGILL 4, SIGFPE 8, SIGSEGV 11, SIGTERM 15, SIGBREAK 21,
SIGABRT 22 numbering; handlers are ms_abi), _beginthread/_beginthreadex/_endthread/_endthreadex
(via `aw_create_thread`), _lock/_unlock, _getptd? (stub), _assert/_wassert (print + abort),
_purecall, terminate/set_terminate/_set_se_translator (store only), operator new/delete/new[]/
delete[] (`??2@YAPEAX_K@Z`, `??3@YAXPEAX@Z`, `??_U@YAPEAX_K@Z`, `??_V@YAXPEAX@Z`, nothrow variants),
_callnewh, _CrtSetReportMode/_CrtSetDbgFlag/_CrtDbgReport stubs, __iob_func, _get_stream_buffer_pointers.
NOT yours: `__C_specific_handler`, `_setjmp`, `_setjmpex`, `setjmp`, `longjmp`, `_local_unwind`,
`__CxxFrameHandler*`, `_CxxThrowException` (→ package C).

Tests: MinGW C programs exercising printf/scanf edge cases (compare against a table of expected
Windows outputs), file I/O incl. `fopen` with backslash paths, `_stat`, `_findfirst`, time
functions, qsort with a program comparator, atexit order, threads via `_beginthreadex`, and a
`-municode` wmain program. Also an MSVC-ABI (`ucrtbase` style) test built with
`x86_64-w64-mingw32-gcc -mcrtdll=ucrt` (mingw supports linking against ucrt).

### C. SEH + advapi32 + shell32 — owns `seh.c`, `advapi32.c`, `shell32.c`

SEH (Windows x64 exception model, see Wine `dlls/ntdll/signal_x86_64.c` & `unwind.c` for reference):
* Hooks: `aw_seh_init()`, `int aw_seh_signal(int sig, siginfo_t*, void *ucontext)` (convert the
  signal to an EXCEPTION_RECORD — SIGSEGV/SIGBUS → 0xC0000005 with read/write flag + address,
  SIGFPE → 0xC0000094/0xC0000095/float codes, SIGILL → 0xC000001D, SIGTRAP/int3 → 0x80000003,
  stack overflow → 0xC00000FD — build a CONTEXT from the ucontext, dispatch; if a handler returns
  EXCEPTION_CONTINUE_EXECUTION write the CONTEXT back to the ucontext and return 1; return 0 if
  the faulting RIP is not in the PE image so start.c prints its crash dump).
* kernel32/ntdll exports: RtlCaptureContext (asm), RtlLookupFunctionEntry (binary search in the
  image `.pdata`; NULL for runtime code), RtlVirtualUnwind (all UNWIND_CODEs, chained info,
  epilogue detection, frame registers, machine frames), RtlUnwindEx, RtlUnwind, RtlRestoreContext,
  RtlRaiseException, RaiseException, RtlPcToFileHeader, RtlAddFunctionTable/RtlDeleteFunctionTable/
  RtlInstallFunctionTableCallback (dynamic tables), SetUnhandledExceptionFilter,
  UnhandledExceptionFilter, AddVectoredExceptionHandler/RemoveVectoredExceptionHandler (+Continue
  variants), RtlUnwind in ntdll namespace too, NtCurrentTeb? (ntdll), RtlNtStatusToDosError.
  Dispatch walks frames with unwind info, calls language handlers (ms_abi) for EHANDLER/UHANDLER;
  when the walk reaches a frame outside the PE image (our runtime: e.g. a WndProc called from
  user32), stop and treat as unhandled: call the unhandled exception filter, then print the
  crash report and exit with the exception code.
* msvcrt-namespace exports: `__C_specific_handler` (scope tables, filters, `__finally` via unwind),
  `_setjmp`/`_setjmpex`/`setjmp` (MSVC `_JUMP_BUFFER` layout incl. Frame, MxCsr, FpCsr, Xmm6-15),
  `longjmp` (restore directly; with a non-zero Frame do an RtlUnwind-style unwind so `__finally`
  blocks run), `_local_unwind`, `__CxxFrameHandler3`, `__CxxFrameHandler4` (stretch: MSVC C++ EH;
  at least `__CxxFrameHandler3` + `_CxxThrowException` + `__std_terminate` + `__current_exception`
  + `__current_exception_context` + `__std_exception_copy/destroy` + `__std_type_info_name`/
  `_compare`, `__RTDynamicCast`, `_purecall` is B's).
* Verify MinGW g++ C++ exceptions work (libgcc's SEH unwinder uses RaiseException,
  RtlUnwindEx, RtlVirtualUnwind, RtlLookupFunctionEntry, RtlCaptureContext, and
  `_GCC_specific_handler` inside the exe): tests with throw/catch across frames, destructors,
  rethrow, `std::terminate`; plus C `__try/__except`-style via MinGW's `__try1`? (or clang `-fms-extensions`
  test built with clang-cl if possible), signal → SEH (null deref caught by a vectored handler
  or by SetUnhandledExceptionFilter).

advapi32 (registry persisted under `~/.anywin/registry/`, e.g. one text file per hive in a
`.reg`-like format, rewritten atomically on change, guarded by a mutex; case-insensitive keys and
value names, preserve original case): RegOpenKey(Ex)A/W, RegCreateKey(Ex)A/W, RegCloseKey,
RegQueryValue(Ex)A/W (exact Win32 size semantics: ERROR_MORE_DATA 234, size query with NULL data,
REG_SZ nul termination), RegSetValue(Ex)A/W, RegGetValueA/W (RRF_RT_* filtering), RegDeleteValueA/W,
RegDeleteKey(Ex)A/W, RegDeleteTreeA/W, RegEnumKey(Ex)A/W, RegEnumValueA/W, RegQueryInfoKeyA/W,
RegFlushKey, RegOpenCurrentUser, RegConnectRegistry (local only), RegNotifyChangeKeyValue (stub);
predefined keys HKEY_CLASSES_ROOT 0x80000000 … HKEY_CURRENT_CONFIG 0x80000005; A functions convert
REG_SZ/EXPAND_SZ/MULTI_SZ between UTF-8 and stored UTF-16. Seed a few read-only values programs
query (HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion ProductName/CurrentBuild…).
**Run-key integration:** writing a value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
also writes `~/.config/autostart/<value>.desktop` with `Exec=` the Unix path of the command
(strip quotes/args properly; map `Z:\…` paths); deleting the value removes the file. (This makes
todo.exe's "Start with Windows" checkbox work on Linux.)
Also: GetUserNameA/W, GetUserNameExA/W (via secur32 namespace alias), OpenProcessToken/
OpenThreadToken/GetTokenInformation (TokenElevation → not elevated, TokenUser minimal SID),
AllocateAndInitializeSid/FreeSid/CheckTokenMembership (FALSE)/EqualSid/CopySid/GetLengthSid/
IsValidSid/ConvertSidToStringSidA/W, LookupAccountSidA/W, InitializeSecurityDescriptor/
SetSecurityDescriptorDacl (TRUE), CryptAcquireContextA/W/CryptReleaseContext/CryptGenRandom,
SystemFunction036 (RtlGenRandom; getrandom), CryptCreateHash/HashData/GetHashParam/DestroyHash for
MD5/SHA1/SHA256 (small implementations), RegisterEventSourceA/W/ReportEventA/W/
DeregisterEventSource (log via TRACE), OpenSCManager/OpenService/… (fail with
ERROR_ACCESS_DENIED), EventRegister/EventUnregister/EventWrite* (succeed, no-op),
GetCurrentHwProfile, ImpersonateSelf/RevertToSelf, LookupPrivilegeValueA/W, AdjustTokenPrivileges.

shell32: Shell_NotifyIconA/W (NIM_ADD/MODIFY/DELETE/SETFOCUS/SETVERSION; one tray icon per
(hWnd,uID) using `plat_tray_add` + `plat_tray_draw` with the `aw_icon` pixels (fallback: a
generated generic icon); tray X events arrive through user32's event loop — coordinate with user32
by exporting `int aw_tray_handle_event(const aw_plat_event *ev)` (returns 1 if the event's window
is a tray icon) which user32's event dispatcher must call first. Map clicks to
`PostMessage(hWnd, uCallbackMessage, uID, WM_LBUTTONDOWN/UP/LBUTTONDBLCLK/RBUTTONDOWN/UP/
MOUSEMOVE/WM_CONTEXTMENU)` (NOTIFYICON_VERSION_4 packing when set), balloon tips (NIF_INFO) via
`notify-send` if present, tooltips ignored). SHGetFolderPathA/W, SHGetSpecialFolderPathA/W,
SHGetKnownFolderPath (FOLDERID_* GUIDs; memory via CoTaskMemAlloc), SHGetFolderLocation/
SHGetPathFromIDListA/W (minimal PIDL = path), SHCreateDirectoryExA/W, SHFileOperationA/W (copy/
move/delete/rename, no UI), ShellExecuteA/W, ShellExecuteExA/W ("open" on URLs/documents →
`xdg-open`; executables → CreateProcess), CommandLineToArgvW, DragAcceptFiles/DragQueryFileA/W/
DragFinish (no-op/0), ExtractIconA/W/ExtractIconExA/W (from own resources), SHAppBarMessage (0),
SHBrowseForFolderA/W (NULL), IsUserAnAdmin (FALSE), SHGetFileInfoA/W (minimal), SHChangeNotify (no-op),
FindExecutableA/W.

### D. X11 platform + fonts — owns `x11.c`, `font.c`

x11.c — raw X11 protocol client over a Unix socket (no libX11/libxcb):
* Connect: parse `$DISPLAY` (`:N`, `:N.S`, `unix:N`; TCP `host:N` optional), try abstract socket
  `@/tmp/.X11-unix/XN` then the path socket; auth from `$XAUTHORITY` or `~/.Xauthority`
  (MIT-MAGIC-COOKIE-1, family Local/hostname matching or wildcard). Parse setup: resource-id
  base/mask, root window, root depth/visual (require TrueColor 24/32 bpp; handle byte order),
  min/max keycode, maximum-request-length (use BIG-REQUESTS if available or split PutImage by rows).
* Request writer with buffering; reply/event/error reader that queues events while waiting for a
  reply with a given sequence number. Errors are logged with TRACE, never fatal.
* InternAtom cache (WM_PROTOCOLS, WM_DELETE_WINDOW, _NET_WM_NAME, UTF8_STRING, _NET_WM_ICON,
  _NET_WM_WINDOW_TYPE(_DIALOG/_NORMAL/_POPUP_MENU/_DROPDOWN_MENU/_TOOLTIP), _NET_WM_STATE(_HIDDEN,
  _MAXIMIZED_VERT/HORZ), _NET_ACTIVE_WINDOW, _NET_WM_PID, WM_CHANGE_STATE, CLIPBOARD, TARGETS,
  _NET_SYSTEM_TRAY_S<screen>, _NET_SYSTEM_TRAY_OPCODE, _XEMBED_INFO, _XEMBED, _MOTIF_WM_HINTS, …).
* Windows: CreateWindow (InputOutput, root visual, event mask for exposure, structure, key,
  button, pointer motion, enter/leave, focus; background None to avoid flicker; override-redirect
  for POPUP), WM_CLASS/WM_NAME/_NET_WM_NAME, WM_PROTOCOLS(WM_DELETE_WINDOW), WM_HINTS (input),
  WM_NORMAL_HINTS (position/size; min=max when not resizable), WM_TRANSIENT_FOR, _NET_WM_PID,
  _NET_WM_ICON, map/unmap, ConfigureWindow, _NET_ACTIVE_WINDOW client message + SetInputFocus,
  iconify via WM_CHANGE_STATE, maximize via _NET_WM_STATE. Track each window's root position from
  ConfigureNotify (translate synthetic vs real events; use TranslateCoordinates when needed) so
  `plat_window_get_pos` is right under reparenting WMs.
* Drawing: one GC; PutImage ZPixmap from the surface rect (MIT-SHM optional for speed).
* Input: keycode→keysym via GetKeyboardMapping (refresh on MappingNotify), modifier map via
  GetModifierMapping; keysym → Windows VK table (letters/digits, F1–F24, arrows, Home/End/PgUp/
  PgDn/Ins/Del, numpad, Shift/Ctrl/Alt L/R → VK_SHIFT/VK_CONTROL/VK_MENU, Win keys, OEM keys
  `;=,-./\`[]'` per US layout, Caps/Num/Scroll lock, Pause, PrintScreen, Apps), scancode =
  keycode-8 (+0x100 for extended keys), UTF-8 text from keysym (Latin-1 + Unicode keysyms
  0x01000000+, control chars for Return/BackSpace/Tab/Escape, Ctrl+letter → control char),
  auto-repeat detection (KeyRelease immediately followed by KeyPress with same time/keycode),
  buttons 4/5/6/7 → PEV_WHEEL, 8/9 → X1/X2, double-click detection is user32's job.
* Cursors: core "cursor" font glyph cursors mapped from IDC_ARROW/IBEAM/WAIT/CROSS/UPARROW/SIZE*/
  NO/HAND/APPSTARTING/HELP.
* `plat_poll` uses `poll()` on the X fd and a self-pipe for `plat_wakeup`.
* Grab: GrabPointer/GrabKeyboard for popup menus.
* Clipboard: own CLIPBOARD and answer SelectionRequest for UTF8_STRING/STRING/TARGETS (handled
  inside the event reader, never surfaced); `plat_clipboard_get` converts the selection
  (ConvertSelection to a property, wait ≤1 s for SelectionNotify, GetProperty, INCR optional).
* System tray (XEmbed): find the owner of `_NET_SYSTEM_TRAY_S<screen>`, create a small window
  (size from the tray's `_NET_SYSTEM_TRAY_ICON_SIZE` or 22–24 px; background ParentRelative or
  `_NET_SYSTEM_TRAY_VISUAL` ARGB visual if offered — then use a colormap and 32-bit depth), set
  `_XEMBED_INFO` (version 0, flags XEMBED_MAPPED), send SYSTEM_TRAY_REQUEST_DOCK; `plat_tray_draw`
  scales the ARGB icon to the window size and composites against the background (with ARGB visual:
  premultiplied alpha; else ParentRelative + draw only opaque-enough pixels). Re-dock when the tray
  manager restarts (MANAGER client message on root) — optional.
* Test with a native Linux harness (`gcc -Iruntime tests/x11_harness.c runtime/x11.c …`) under
  `xvfb-run -a`: create windows, put an image, receive synthetic input from `xdotool`, screenshot
  with `import -window root` and check pixels.

font.c — TrueType renderer:
* Locate fonts at first use: map face names → files: "Segoe UI", "Tahoma", "MS Shell Dlg(2)",
  "Microsoft Sans Serif", "Arial", "Helvetica", "Verdana", "Calibri", "System", default → sans
  (prefer Noto Sans, then DejaVu Sans, then Liberation Sans); "Courier New", "Consolas",
  "Lucida Console", "Terminal", "Fixedsys", FIXED_PITCH → mono (DejaVu Sans Mono, Liberation Mono,
  Noto Sans Mono); "Times New Roman", "Georgia", "Cambria" → serif (Liberation Serif, DejaVu Serif,
  Noto Serif); weight ≥ 600 → Bold file, italic → Italic/Oblique file. Search
  `/usr/share/fonts/**`, `/usr/local/share/fonts/**`, `~/.local/share/fonts/**`, `~/.fonts`
  (+ `ANYWIN_FONT_DIR`); synthesize bold (1px smear) / italic (shear) when files are missing.
* Parse `head`, `maxp`, `cmap` (formats 4 and 12; platform 3 enc 1/10, platform 0), `hhea`, `hmtx`,
  `loca`, `glyf` (simple + composite glyphs with ARG_1_AND_2_ARE_WORDS, ARGS_ARE_XY_VALUES,
  WE_HAVE_A_SCALE / X_AND_Y_SCALE / TWO_BY_TWO, MORE_COMPONENTS), `OS/2` (usWinAscent/usWinDescent →
  Windows tmAscent/tmDescent; xAvgCharWidth → tmAveCharWidth), `kern` optional. TTC collections
  (offset table selection) supported. CFF/OTF outlines unsupported → skip that file.
* Size: lfHeight < 0 → ppem = -lfHeight; > 0 → scale so winAscent+winDescent = lfHeight; 0 → cell 16.
  Metrics: ascent = ceil(winAscent·s), descent = ceil(winDescent·s), internal leading =
  height − ppem, ave width from xAvgCharWidth (fallback: advance of 'x'), max width.
* Rasterizer: flatten quadratic Béziers into lines (adaptive), signed-area coverage accumulation
  (font-rs style) → 8-bit AA; light vertical hinting optional (snap baseline/x-height); glyph cache
  per (font,size,codepoint). Advances rounded to integer pixels.
* Fallback when no font file exists: a built-in bitmap font (e.g. hand-encoded 6x13 or 8x16 for
  ASCII 32–126 + box for others) scaled by integer factors, so text always renders.
* Fallback for missing glyphs (CJK, symbols): try other installed fonts (Noto CJK `.ttc`, DejaVu)
  before the box.
* Test with a native harness rendering strings to PGM files; check metrics against known
  values (e.g. DejaVu Sans at -12: ascent 12, descent 3).

### E. gdi32 (+ regions, surfaces, user32 drawing helpers) — owns `gdi*.c`, `region.c`, `surface.c`, `drawtext.c`

* Implement `surface_*` and `rgn_*` from gui.h, and the GDI internals (`gdi_create_window_dc`, …).
* Object model: handle table (32-bit values with a type tag in the high bits like Windows), objects:
  DC (window DC, memory DC, metafile-less), pen (cosmetic solid/dash/dot/null, width), brush (solid,
  null/hollow, hatched, pattern/DIB pattern basic), font (LOGFONTA/W → `font_open`), bitmap (DDB =
  32bpp surface; DIB section with user-visible bits: 32bpp bottom-up/top-down maps directly via
  stride; 24/16/8/4/1bpp sections keep a shadow surface synchronized before reads/after writes —
  do conversion at GdiFlush/BitBlt/select time), region (HRGN over `aw_region`), palette (stub).
  Stock objects: WHITE/LTGRAY/GRAY/DKGRAY/BLACK/NULL/DC brush, WHITE/BLACK/NULL/DC pen, system/
  ANSI/OEM/DEFAULT_GUI fonts (DEFAULT_GUI_FONT = "Segoe UI" -12 → sans), DEFAULT_PALETTE; stock
  objects are never deleted.
* DC state: selected objects, text/bk color, bk mode, ROP2 (R2_COPYPEN, R2_XORPEN, R2_NOT…),
  text align (TA_LEFT/RIGHT/CENTER, TOP/BASELINE/BOTTOM, UPDATECP), current position, clip region
  (`SelectClipRgn`, `IntersectClipRect`, `ExcludeClipRect`, `GetClipBox`, `GetClipRgn`) intersected
  with the visible region, viewport/window origin & extents (MM_TEXT fully, MM_ANISOTROPIC/
  ISOTROPIC basic scaling), SaveDC/RestoreDC, stretch mode, polyfill mode, brush origin,
  graphics mode, DC brush/pen colors, GetCurrentObject.
* Drawing API (A/W where relevant): MoveToEx, LineTo, Polyline(To), PolyPolyline, Polygon,
  PolyPolygon, Rectangle, RoundRect, Ellipse, Arc, ArcTo, Chord, Pie, AngleArc, PolyBezier(To),
  FillRect (user32), FrameRect (user32), InvertRect (user32), FillRgn, FrameRgn, PaintRgn,
  InvertRgn, PatBlt (PATCOPY/PATINVERT/DSTINVERT/BLACKNESS/WHITENESS), BitBlt, StretchBlt,
  MaskBlt (basic), TransparentBlt, AlphaBlend (msimg32 namespace; AC_SRC_ALPHA premultiplied),
  GradientFill (msimg32), SetPixel(V), GetPixel, ExtFloodFill/FloodFill, SetDIBits, GetDIBits,
  SetDIBitsToDevice, StretchDIBits (1/4/8/16/24/32 bpp, BI_RGB/BI_BITFIELDS, top-down/bottom-up,
  palettes), CreateDIBSection, CreateDIBitmap, CreateBitmap(Indirect), CreateCompatibleBitmap,
  CreateCompatibleDC, CreateDCA/W("DISPLAY"), DeleteDC, GetObjectA/W (BITMAP/DIBSECTION/LOGPEN/
  EXTLOGPEN/LOGBRUSH/LOGFONT), GetObjectType, SelectObject, DeleteObject, GetStockObject,
  CreatePen(Indirect), ExtCreatePen, CreateSolidBrush, CreateHatchBrush, CreatePatternBrush,
  CreateBrushIndirect, CreateDIBPatternBrushPt, CreateFontA/W, CreateFontIndirectA/W(ExW),
  SetTextColor/GetTextColor, SetBkColor/GetBkColor, SetBkMode/GetBkMode, SetROP2/GetROP2,
  SetTextAlign/GetTextAlign, SetTextCharacterExtra, SetTextJustification, TextOutA/W, ExtTextOutA/W
  (ETO_OPAQUE/ETO_CLIPPED, lpDx), PolyTextOutA/W, GetTextExtentPoint(32)A/W,
  GetTextExtentExPointA/W, GetCharWidth(32)A/W, GetCharABCWidthsA/W, GetTextMetricsA/W,
  GetOutlineTextMetricsA/W (basic), GetTextFaceA/W, GetFontData (from the TTF if possible, else
  GDI_ERROR), EnumFontFamiliesA/W(Ex) and EnumFontsA/W (report a fixed list: Segoe UI, Arial,
  Tahoma, Courier New, Times New Roman, Consolas…; callbacks ms_abi), AddFontResourceExA/W (accept
  .ttf file → register with font.c if feasible), GetDeviceCaps (HORZRES/VERTRES from
  plat_screen_size, LOGPIXELSX/Y 96, BITSPIXEL 32, PLANES 1, NUMCOLORS -1, RASTERCAPS,
  TECHNOLOGY DT_RASDISPLAY, SIZEPALETTE 0, HORZSIZE/VERTSIZE mm), GetDCOrgEx, Get/SetViewportOrgEx,
  Get/SetWindowOrgEx, Set/GetViewportExtEx, Set/GetWindowExtEx, SetMapMode/GetMapMode,
  OffsetViewportOrgEx, DPtoLP, LPtoDP, SetStretchBltMode, SetPolyFillMode, SetBrushOrgEx,
  SetDCBrushColor/SetDCPenColor, GetDCBrushColor/GetDCPenColor, SaveDC/RestoreDC, Region API
  (CreateRectRgn(Indirect), CreateRoundRectRgn and CreateEllipticRgn (approximate with rect bands),
  CreatePolygonRgn (scanline bands), CombineRgn (all modes), EqualRgn, OffsetRgn, GetRgnBox,
  PtInRegion, RectInRegion, SetRectRgn, GetRegionData, ExtCreateRegion), SelectClipRgn,
  ExtSelectClipRgn, GetClipRgn, GetRandomRgn (SYSRGN), BeginPath/EndPath/StrokePath/FillPath/
  StrokeAndFillPath/AbortPath (polygon paths), GdiFlush, GdiSetBatchLimit, SetGraphicsMode,
  SetWorldTransform/ModifyWorldTransform/GetWorldTransform (translation+scale), CreatePalette/
  SelectPalette/RealizePalette/GetSystemPaletteEntries (stubs), GetNearestColor,
  SetLayout/GetLayout (LTR only), CreateICA/W, StartDoc/StartPage/EndPage/EndDoc (fail).
* user32-namespace drawing helpers (these live in E's files but use `API(user32, …)`): DrawTextA/W/
  ExA/ExW (DT_LEFT/CENTER/RIGHT/TOP/VCENTER/BOTTOM/SINGLELINE/WORDBREAK/CALCRECT/NOPREFIX/
  END_ELLIPSIS/PATH_ELLIPSIS/WORD_ELLIPSIS/EXPANDTABS/NOCLIP/HIDEPREFIX/EDITCONTROL; `&` mnemonic
  underline), TabbedTextOutA/W, GetTabbedTextExtentA/W, GrayStringA/W, DrawStateA/W (text/icon/
  bitmap, DSS_DISABLED embossed), FillRect, FrameRect, InvertRect, DrawFocusRect (dotted XOR),
  DrawEdge (all BDR_/EDGE_/BF_ flags with Win2k/XP-classic colors from `aw_syscolor`),
  DrawFrameControl (buttons, check boxes, radio buttons, scroll arrows, menu check/bullet/arrow,
  caption buttons), DrawCaption (simple), and rect helpers SetRect, SetRectEmpty, CopyRect,
  InflateRect, OffsetRect, IntersectRect, UnionRect, SubtractRect, IsRectEmpty, PtInRect, EqualRect.
* Text rendering: glyph coverage blended with the text color over the destination (gamma-correct
  blend optional), honoring the DC clip; opaque background when OPAQUE; underline/strikeout lines.
* Coordinates: all drawing converts logical→device (mapping + window origin) then offsets by the
  DC org; clip in device space.
* Tests: native harness drawing into memory DCs (fonts via D's font.c when available) and dumping
  PPM files; visual check; unit tests for regions (random rect ops vs a bitmap oracle).

### F. user32 core — owns `user32*.c` (split e.g. `user32_wnd.c`, `user32_msg.c`, `user32_input.c`, `user32_paint.c`, `user32_nc.c`, `user32_misc.c`)

* `aw_user_init()`: `plat_init()`; register built-ins via `aw_controls_register()`,
  `aw_dialogs_register()`, `aw_menus_register()`; set `aw_gui_atexit` to destroy X windows cleanly.
  If no display: window creation fails gracefully with an error (console programs never touch X).
* Window objects: handle table (HWND = 32-bit values, e.g. `0x10000 | idx<<1`), class (atom,
  style, proc, unicode flag, cbClsExtra/cbWndExtra, hInstance, icon/iconSm/cursor/brush (incl.
  `(HBRUSH)(COLOR_x+1)`), menu name), styles, exstyle, text (UTF-16 internally), id/HMENU,
  parent/owner, z-ordered children, rects (window rect in parent-client coords, client rect),
  extra bytes, user data, props, scroll info, update region + erase/NC flags, visible/enabled,
  per-top-level X window + backbuffer surface.
* Classes: RegisterClass(Ex)A/W, UnregisterClassA/W, GetClassInfo(Ex)A/W, GetClassNameA/W,
  RealGetWindowClassA/W, Get/SetClassLong(Ptr)A/W, Get/SetClassWord, case-insensitive names,
  global atoms (GlobalAddAtom… can live here: AddAtomA/W, GlobalAddAtomA/W, GlobalFindAtom,
  GlobalGetAtomName, DeleteAtom — kernel32 namespace OK), CS_DBLCLKS, CS_HREDRAW/VREDRAW,
  CS_OWNDC/CS_CLASSDC (cache a DC), CS_NOCLOSE.
* Creation: CreateWindowExA/W (CW_USEDEFAULT for x/y/w/h with cascading defaults, HWND_MESSAGE
  parent, WS_CHILD vs top-level, WM_GETMINMAXINFO, WM_NCCREATE → WM_NCCALCSIZE → WM_CREATE with
  correct CREATESTRUCTA/W, abort on -1/FALSE, WS_VISIBLE → show after WM_CREATE, WM_PARENTNOTIFY,
  initial WM_SIZE/WM_MOVE, WS_EX_TOPMOST, MDI (WS_EX_MDICHILD) optional), DestroyWindow (children
  first, WM_DESTROY parent-first then WM_NCDESTROY child-first, owned popups, focus/capture/
  active fixes, timers killed, menu destroyed), CloseWindow.
* Geometry: MoveWindow, SetWindowPos (all SWP_* flags incl. NOZORDER/NOACTIVATE/FRAMECHANGED/
  SHOWWINDOW/HIDEWINDOW, HWND_TOP/BOTTOM/TOPMOST, WM_WINDOWPOSCHANGING/CHANGED → WM_MOVE/WM_SIZE
  via DefWindowProc), GetWindowRect, GetClientRect, ClientToScreen, ScreenToClient, MapWindowPoints,
  WindowFromPoint, ChildWindowFromPoint(Ex), RealChildWindowFromPoint, AdjustWindowRect(Ex)(ForDpi)
  (only the menu bar — see §5), GetWindowPlacement/SetWindowPlacement, BeginDeferWindowPos/
  DeferWindowPos/EndDeferWindowPos, GetWindowInfo, ShowWindow (all SW_*; SW_HIDE unmaps X window;
  SW_SHOWDEFAULT/NORMAL; minimize/maximize through the WM), ShowWindowAsync, IsIconic, IsZoomed,
  IsWindowVisible (ancestors), BringWindowToTop, SetForegroundWindow/GetForegroundWindow,
  SetActiveWindow/GetActiveWindow (WM_ACTIVATE/WM_ACTIVATEAPP/WM_NCACTIVATE), FlashWindow(Ex),
  LockSetForegroundWindow/AllowSetForegroundWindow, AnimateWindow (just show), SetWindowRgn (store).
* Hierarchy: GetParent, SetParent, GetAncestor, GetWindow (GW_*), GetTopWindow, GetNextWindow,
  GetDesktopWindow (a pseudo window covering the screen), GetShellWindow, IsChild, IsWindow,
  IsWindowEnabled, EnableWindow (WM_ENABLE; disabling removes focus), FindWindowA/W(ExA/ExW)
  (top-level windows of this process; class/title match), EnumWindows, EnumChildWindows,
  EnumThreadWindows (callbacks ms_abi), GetWindowThreadProcessId, GetLastActivePopup,
  IsWindowUnicode, GetDlgCtrlID, SetWindowContextHelpId stub.
* Window data: Get/SetWindowLong(Ptr)A/W (GWL_STYLE (+WM_STYLECHANGING/ED), GWL_EXSTYLE,
  GWLP_WNDPROC (A/W thunking: returns a value CallWindowProc understands — e.g. tagged handle for
  cross-charset procs), GWLP_USERDATA, GWLP_ID, GWLP_HINSTANCE, GWLP_HWNDPARENT, extra bytes
  incl. DWLP_MSGRESULT/DWLP_DLGPROC/DWLP_USER offsets used by dialogs), Get/SetWindowWord,
  Get/SetWindowTextA/W (+ cross-thread rule ignored), GetWindowTextLengthA/W, InternalGetWindowText,
  SetPropA/W/GetPropA/W/RemovePropA/W/EnumPropsA/W(Ex).
* Messages: per-thread queue (posted messages, quit flag, timers, paint, input) and a global
  router for SendMessage to other threads (synchronous handoff with wakeup via plat_wakeup /
  condvar). GetMessageA/W, PeekMessageA/W (PM_REMOVE/NOREMOVE/NOYIELD, filters, QS_* masks),
  WaitMessage, MsgWaitForMultipleObjects(Ex) (combine kernel waits with X fd polling — e.g. a
  helper that polls with short timeouts or uses aw_wait_multiple + plat_wakeup), TranslateMessage
  (WM_KEYDOWN → WM_CHAR/WM_SYSCHAR/WM_DEADCHAR from the event's UTF-8 text, delivered as UTF-16 to
  W windows and UTF-8 bytes (one WM_CHAR per byte) to A windows), DispatchMessageA/W (WM_TIMER
  with TIMERPROC calls the proc), SendMessageA/W, SendMessageTimeoutA/W, SendNotifyMessageA/W,
  SendMessageCallbackA/W, PostMessageA/W (HWND_BROADCAST to top-levels), PostThreadMessageA/W,
  PostQuitMessage, GetMessageTime/Pos/ExtraInfo, InSendMessage(Ex), ReplyMessage, GetQueueStatus,
  GetInputState, RegisterWindowMessageA/W (≥0xC000, shared with atoms), CallWindowProcA/W,
  DefWindowProcA/W, BroadcastSystemMessage (no-op), SetWindowsHookExA/W/UnhookWindowsHookEx/
  CallNextHookEx (WH_KEYBOARD/WH_MOUSE/WH_GETMESSAGE/WH_CALLWNDPROC/WH_MSGFILTER/WH_CBT basics,
  LL hooks accepted but never called), SetWinEventHook stub, `aw_modal_loop()`.
* A⇄W message conversion when the caller's charset differs from the target proc's: WM_SETTEXT,
  WM_GETTEXT, WM_GETTEXTLENGTH, WM_NCCREATE/WM_CREATE (CREATESTRUCT strings), WM_CHAR/WM_SYSCHAR/
  WM_DEADCHAR/WM_IME_CHAR, WM_MDICREATE, EM_REPLACESEL, EM_GETLINE, EM_SETPASSWORDCHAR,
  CB_ADDSTRING/INSERTSTRING/FINDSTRING(EXACT)/SELECTSTRING/GETLBTEXT/GETLBTEXTLEN/DIR,
  LB_ADDSTRING/INSERTSTRING/FINDSTRING(EXACT)/SELECTSTRING/GETTEXT/GETTEXTLEN/DIR (only when the
  list has strings: !owner-draw or LBS_HASSTRINGS/CBS_HASSTRINGS), WM_SETTINGCHANGE string,
  WM_DEVMODECHANGE, WM_ASKCBFORMATNAME. (LVM_*A vs LVM_*W, TVM_*, TCM_*, HDM_* differ by message
  number and are handled by the controls themselves.)
* Timers: SetTimer/KillTimer (window and thread timers, TIMERPROC, ids, USER_TIMER_MINIMUM 10ms),
  SetCoalescableTimer; WM_TIMER generated when the queue is otherwise empty (low priority like Windows).
* Painting: update regions per window (`aw_region`), InvalidateRect/InvalidateRgn (erase flag),
  ValidateRect/ValidateRgn, UpdateWindow (immediate WM_PAINT if update region), RedrawWindow (all
  RDW_* flags incl. ALLCHILDREN/UPDATENOW/ERASENOW/FRAME/INTERNALPAINT), GetUpdateRect/GetUpdateRgn,
  BeginPaint (WM_NCPAINT handled internally for NC area, WM_ERASEBKGND if needed → DefWindowProc
  fills class background, PAINTSTRUCT, DC clipped to update ∩ visible client), EndPaint, GetDC/
  GetDCEx (DCX_WINDOW/CACHE/CLIPCHILDREN/CLIPSIBLINGS/PARENTCLIP/INTERSECTRGN/EXCLUDERGN)/
  GetWindowDC/ReleaseDC, WindowFromDC, LockWindowUpdate, ScrollWindow(Ex) (move pixels inside the
  backbuffer + invalidate exposed area; SW_SCROLLCHILDREN, SW_INVALIDATE, SW_ERASE), ScrollDC,
  ExcludeUpdateRgn, GetWindowRgn, PrintWindow, WM_PRINT/WM_PRINTCLIENT in DefWindowProc.
  Visible region = window ∩ all ancestors' clients − children (always clip children) − higher
  siblings. WM_PAINT generated when the queue is empty, parents before children. After dispatch
  of paint/any message, flush dirty backbuffer areas to X with `plat_put` when the thread goes idle
  (before blocking in GetMessage/WaitMessage), and on Expose just re-put from the backbuffer.
  Resizing a top-level (CONFIGURE event) resizes its surface, sends WM_SIZE/WM_MOVE (via
  WM_WINDOWPOSCHANGED), invalidates (CS_HREDRAW/VREDRAW → everything).
* Non-client area of child windows: WS_BORDER (1px), WS_EX_CLIENTEDGE (2px sunken),
  WS_EX_STATICEDGE, WS_EX_WINDOWEDGE/WS_DLGFRAME/WS_THICKFRAME (raised), WS_CAPTION on children
  (simple caption), standard scroll bars WS_VSCROLL/WS_HSCROLL (arrows, track, thumb; mouse
  tracking with auto-repeat timers → WM_VSCROLL/WM_HSCROLL SB_LINEUP/DOWN/PAGEUP/DOWN/THUMBTRACK/
  THUMBPOSITION/ENDSCROLL), SetScrollInfo/GetScrollInfo/SetScrollPos/GetScrollPos/SetScrollRange/
  GetScrollRange/ShowScrollBar/EnableScrollBar/GetScrollBarInfo; top-levels: menu bar via
  menu.c hooks. WM_NCCALCSIZE, WM_NCHITTEST (HTCLIENT/HTVSCROLL/HTHSCROLL/HTMENU/HTBORDER/…),
  WM_NCPAINT, WM_NCMOUSEMOVE/NCLBUTTONDOWN etc. delivered for NC hits.
* Input: X events → messages: mouse to the window under the pointer (deepest visible enabled child
  by hit testing with WM_NCHITTEST; HTTRANSPARENT passes to next), or to the capture window
  (SetCapture/ReleaseCapture/GetCapture, WM_CAPTURECHANGED; capture on a POPUP top-level →
  `plat_grab`), client vs NC messages, double clicks (CS_DBLCLKS, GetDoubleClickTime 500 ms,
  4 px), WM_MOUSEWHEEL/WM_MOUSEHWHEEL to the focus window (screen coords, DefWindowProc propagates
  to parent), WM_SETCURSOR (DefWindowProc: class cursor / HT-specific / ask parent first),
  WM_MOUSEACTIVATE, WM_MOUSELEAVE/WM_MOUSEHOVER via TrackMouseEvent (+ comctl32 `_TrackMouseEvent`
  alias), WM_XBUTTON*; keyboard to the focus window (SetFocus/GetFocus with WM_KILLFOCUS/
  WM_SETFOCUS; focus follows activation), WM_KEYDOWN/UP and WM_SYSKEYDOWN/UP (Alt / F10) with
  proper lParam (repeat count, scancode, extended, context, previous state, transition),
  key state tracking for GetKeyState (toggle bits)/GetAsyncKeyState/GetKeyboardState/
  SetKeyboardState, MapVirtualKey(Ex)A/W, VkKeyScan(Ex)A/W, GetKeyNameTextA/W, ToAscii(Ex)/
  ToUnicode(Ex), GetKeyboardLayout/GetKeyboardLayoutNameA/W/LoadKeyboardLayout (US),
  ActivateKeyboardLayout, GetKBCodePage, keybd_event/mouse_event/SendInput (inject into own queue),
  BlockInput (FALSE), GetLastInputInfo, GetCursorPos/SetCursorPos (WarpPointer optional)/
  GetPhysicalCursorPos, ClipCursor/GetClipCursor, ShowCursor (counter), SetCursor/GetCursor,
  GetCursorInfo, DefWindowProc: WM_SYSKEYDOWN Alt+F4 → WM_SYSCOMMAND SC_CLOSE → WM_CLOSE,
  F10/Alt → menu bar (`aw_menubar_track`), WM_SYSCOMMAND (SC_CLOSE/MINIMIZE/MAXIMIZE/RESTORE/
  KEYMENU), WM_CLOSE → DestroyWindow, X window close button → WM_SYSCOMMAND SC_CLOSE,
  WM_CONTEXTMENU (shift+F10/apps key/right click; DefWindowProc propagates to parent),
  WM_CTLCOLORMSGBOX/EDIT/LISTBOX/BTN/DLG/SCROLLBAR/STATIC defaults (COLOR_WINDOW/COLOR_3DFACE
  brushes + text colors), WM_GETICON/WM_SETICON (+ `plat_window_set_icon`), WM_SETREDRAW,
  WM_HELP no-op, WM_QUERYENDSESSION, WM_GETFONT? (0), WM_NOTIFYFORMAT (NFR_UNICODE/ANSI by proc).
* Icons/cursors: `aw_icon` objects, LoadIconA/W (IDI_* system icons generated procedurally:
  application, information, warning, error, question, shield; resources: RT_GROUP_ICON → best size
  RT_ICON (BMP DIB 1/4/8/24/32 bpp with AND mask, or PNG — implement a small inflate + PNG decoder,
  or skip PNG entries when a DIB entry exists)), LoadCursorA/W (IDC_* system cursors with `idc`
  set; resource cursors RT_GROUP_CURSOR/RT_CURSOR), LoadImageA/W (IMAGE_ICON/CURSOR/BITMAP from
  resources or files with LR_LOADFROMFILE, LR_DEFAULTSIZE, LR_SHARED; IMAGE_BITMAP → GDI DIB
  section via `__aw_gdi32_CreateDIBSection`), CreateIcon, CreateIconIndirect, GetIconInfo(ExA/W),
  CopyIcon, CopyImage, DestroyIcon, DestroyCursor, DrawIcon, DrawIconEx (via `gdi_draw_argb`),
  CreateIconFromResource(Ex), LookupIconIdFromDirectory(Ex), LoadBitmapA/W (resources + OBM_*
  stubs), `aw_icon_from_handle`. Window cursor shown via `plat_set_cursor(win, idc)` (custom image
  cursors → nearest system shape).
* Clipboard: OpenClipboard/CloseClipboard/EmptyClipboard/SetClipboardData/GetClipboardData
  (CF_TEXT/CF_UNICODETEXT/CF_OEMTEXT synthesized from each other; data is HGLOBAL = pointer from
  kernel32's GlobalAlloc), IsClipboardFormatAvailable, CountClipboardFormats,
  EnumClipboardFormats, RegisterClipboardFormatA/W, GetClipboardOwner, GetOpenClipboardWindow,
  AddClipboardFormatListener (stub), using plat_clipboard_get/set.
* System info: GetSystemMetrics (SM_CXSCREEN/CYSCREEN from plat, SM_CXVSCROLL 17, SM_CYHSCROLL 17,
  SM_CYCAPTION 23, SM_CXBORDER 1, SM_CXEDGE 2, SM_CXFRAME 4, SM_CXICON 32, SM_CXSMICON 16,
  SM_CYMENU 20, SM_CXDOUBLECLK 4, SM_CMOUSEBUTTONS 3, SM_MOUSEWHEELPRESENT, SM_CXMAXIMIZED, SM_REMOTESESSION 0,
  virtual screen, SM_CMONITORS 1, …), GetSystemMetricsForDpi, GetSysColor/`aw_syscolor` (Windows 10
  classic values: COLOR_WINDOW white, COLOR_3DFACE/BTNFACE 0xF0F0F0, COLOR_WINDOWTEXT black,
  COLOR_HIGHLIGHT 0x0078D7, COLOR_HIGHLIGHTTEXT white, COLOR_GRAYTEXT 0x6D6D6D, COLOR_BTNSHADOW
  0xA0A0A0, COLOR_3DHILIGHT white, COLOR_3DDKSHADOW 0x696969, …), GetSysColorBrush (cached),
  SetSysColors (store), SystemParametersInfoA/W (SPI_GETWORKAREA, SPI_GETNONCLIENTMETRICS (with
  LOGFONT "Segoe UI" -12), SPI_GETICONTITLELOGFONT, SPI_GETWHEELSCROLLLINES 3, SPI_GETKEYBOARDDELAY/
  SPEED, SPI_GETMOUSEHOVERTIME/WIDTH, SPI_GETFONTSMOOTHING, SPI_GETHIGHCONTRAST, SPI_GETFLATMENU,
  SPI_GETCLIENTAREAANIMATION…; Set* accepted), GetDpiForWindow/GetDpiForSystem/GetDpiFromDpiAwarenessContext
  (96), SetProcessDPIAware/IsProcessDPIAware/SetProcessDpiAwarenessContext/
  SetThreadDpiAwarenessContext/GetThreadDpiAwarenessContext/GetWindowDpiAwarenessContext/
  AreDpiAwarenessContextsEqual/EnableNonClientDpiScaling/AdjustWindowRectExForDpi (shcore's
  SetProcessDpiAwareness/GetDpiForMonitor too, in a shcore namespace), MonitorFromWindow/Point/Rect,
  GetMonitorInfoA/W, EnumDisplayMonitors, EnumDisplayDevicesA/W, EnumDisplaySettingsA/W(Ex)
  (current mode), ChangeDisplaySettingsA/W (DISP_CHANGE_SUCCESSFUL no-op), GetGUIThreadInfo,
  GetProcessWindowStation/GetUserObjectInformationA/W (visible), OpenInputDesktop/CloseDesktop.
* Misc user32: wsprintfA/W, wvsprintfA/W (own small formatter: %d %u %x %X %s %S %c %lu %ld %hs
  %ls %I64d, width/precision/flags; 1024-char limit), LoadStringA/W (RT_STRING bundles; W with
  nBufferMax 0 returns pointer to the resource), CharUpperA/W, CharLowerA/W, CharUpperBuffA/W,
  CharLowerBuffA/W, CharNextA/W, CharPrevA/W, CharNextExA, IsCharAlphaA/W, IsCharAlphaNumericA/W,
  IsCharUpperA/W, IsCharLowerA/W, CharToOemA/W, OemToCharA/W (+Buff), MessageBeep → plat_bell
  (unless G owns it — G owns MessageBeep), ExitWindowsEx (FALSE), GetWindowModuleFileNameA/W,
  RegisterPowerSettingNotification (stub), RegisterHotKey (FALSE)/UnregisterHotKey,
  GetDoubleClickTime/SetDoubleClickTime, GetCaretBlinkTime, CreateCaret/DestroyCaret/ShowCaret/
  HideCaret/SetCaretPos/GetCaretPos (blinking caret drawn with XOR by a system timer),
  DragDetect, IsHungAppWindow (FALSE), GetAltTabInfo stub, RegisterDeviceNotification (stub),
  GetRawInputData/RegisterRawInputDevices (fail), ChangeWindowMessageFilter(Ex) (TRUE),
  GetComboBoxInfo/GetListBoxInfo (forward to controls via messages), SetLayeredWindowAttributes/
  UpdateLayeredWindow (accept), GetDialogBaseUnits (font-based: 8x16 for Segoe UI 9pt … compute
  from DEFAULT_GUI_FONT average char width/height), plus the simple dialog helpers that don't need
  the dialog manager: GetDlgItem, SendDlgItemMessageA/W, SetDlgItemTextA/W, GetDlgItemTextA/W,
  SetDlgItemInt, GetDlgItemInt, CheckDlgButton, IsDlgButtonChecked, CheckRadioButton.
* Tray integration: before handling an X event, call shell32's `aw_tray_handle_event(&ev)` (C
  defines it; declare it weak so F links alone).
* Tests (once others link; meanwhile with ignore-unresolved): MinGW GUI programs under
  `xvfb-run -a`, driven with `xdotool`, verified with `import` screenshots: window with painted
  shapes/text, child windows overlapping, invalidate/update, timers, keyboard input in a custom
  window, mouse capture, A and W programs, MsgWaitForMultipleObjects with a worker thread posting
  messages.

### G. menus, accelerators, dialogs, MessageBox — owns `menu.c`, `dialog.c`

Menus:
* HMENU table (32-bit handles); items: id, flags (MF_STRING/SEPARATOR/POPUP/CHECKED/GRAYED/
  DISABLED/BITMAP/OWNERDRAW/MENUBARBREAK/MENUBREAK/RIGHTJUSTIFY/RADIOCHECK/DEFAULT), text (UTF-16,
  `&` mnemonics, `\t` accelerator text right-aligned), submenu, item data, bitmaps (checked/
  unchecked/item bitmap HBMMENU_* minimal), MENUINFO (style, max height, background brush).
* API: CreateMenu, CreatePopupMenu, DestroyMenu (recursive), AppendMenuA/W, InsertMenuA/W,
  InsertMenuItemA/W, ModifyMenuA/W, RemoveMenu, DeleteMenu, CheckMenuItem, CheckMenuRadioItem,
  EnableMenuItem, HiliteMenuItem, GetMenuItemCount, GetMenuItemID, GetSubMenu, GetMenuItemInfoA/W,
  SetMenuItemInfoA/W, GetMenuState, GetMenuStringA/W, SetMenuDefaultItem, GetMenuDefaultItem,
  GetMenuInfo/SetMenuInfo, SetMenuItemBitmaps, GetMenuCheckMarkDimensions, GetMenuItemRect,
  MenuItemFromPoint, IsMenu, LoadMenuA/W (RT_MENU standard and extended templates),
  LoadMenuIndirectA/W, GetSystemMenu (return a static menu with Restore/Move/Size/Minimize/
  Maximize/Close; GetSystemMenu(hwnd, TRUE) resets), DrawMenuBar (ask user32 to repaint NC:
  `RedrawWindow(hwnd, NULL, NULL, RDW_FRAME|RDW_INVALIDATE)`), GetMenu/SetMenu are user32 core's
  (they store HMENU and call `aw_menubar_height` → G must provide those hooks), GetMenuBarInfo.
* Popup menu window class "#32768" (registered by `aw_menus_register()`, W proc): TrackPopupMenu/
  TrackPopupMenuEx (TPM_LEFTALIGN/CENTERALIGN/RIGHTALIGN/TOPALIGN/VCENTERALIGN/BOTTOMALIGN,
  TPM_RETURNCMD, TPM_NONOTIFY, TPM_RIGHTBUTTON, TPMPARAMS exclusion rect, keep on screen) creates
  WS_POPUP window(s) (override-redirect via user32), sends WM_ENTERMENULOOP, WM_INITMENU,
  WM_INITMENUPOPUP, WM_MENUSELECT, WM_UNINITMENUPOPUP, WM_EXITMENULOOP, WM_MENUCHAR,
  WM_DRAWITEM/WM_MEASUREITEM for owner-draw items, runs a modal tracking loop (SetCapture on the
  popup → user32 grabs the pointer; clicks outside cancel; keyboard: arrows, Enter, Esc,
  mnemonics, Left/Right for submenus; hover opens submenus after a delay), and finally returns the
  command (TPM_RETURNCMD) or posts WM_COMMAND (WM_SYSCOMMAND for system menus) to the owner.
  EndMenu. Classic Win10-ish look: white background, 0x0078D7-ish highlight (or COLOR_HIGHLIGHT),
  separators, check marks/radio bullets, submenu arrows, gray disabled text, font = the
  non-client menu font (Segoe UI 9pt equivalent).
* Menu bar: `aw_menubar_height/paint/track/aw_menu_is_valid` hooks (gui.h): layout items in rows
  (wrap when too narrow), paint them, track mouse/keyboard (Alt, F10, mnemonics with Alt) opening
  popup submenus and moving between top-level items, sending WM_COMMAND.
* Accelerators: LoadAcceleratorsA/W (RT_ACCELERATOR), CreateAcceleratorTableA/W,
  DestroyAcceleratorTable, CopyAcceleratorTableA/W, TranslateAcceleratorA/W (FVIRTKEY/FSHIFT/
  FCONTROL/FALT/FNOINVERT; WM_KEYDOWN/WM_SYSKEYDOWN/WM_CHAR → WM_COMMAND with HIWORD 1 or
  WM_SYSCOMMAND), TranslateMDISysAccel (FALSE).

Dialogs:
* `aw_dialogs_register()`: class "#32770" (W proc = DefDlgProc logic, cbWndExtra = DLGWINDOWEXTRA
  30 with DWLP_MSGRESULT 0, DWLP_DLGPROC 8, DWLP_USER 16).
* Template parsing: DLGTEMPLATE and DLGTEMPLATEEX (menu, class, title, DS_SETFONT/DS_SHELLFONT
  font with point size/weight/italic/face, items with class atoms 0x80 Button, 0x81 Edit, 0x82
  Static, 0x83 ListBox, 0x84 ScrollBar, 0x85 ComboBox or names, title as string or resource
  ordinal (for icons in statics), creation data), dialog base units from the dialog font
  (average char width of "A–Za–z" / 52 and text height) → MapDialogRect; DS_CENTER/
  DS_CENTERMOUSE/DS_MODALFRAME/DS_FIXEDSYS/DS_CONTROL/DS_3DLOOK/WS_CHILD dialogs.
* CreateDialogParamA/W, CreateDialogIndirectParamA/W, DialogBoxParamA/W, DialogBoxIndirectParamA/W
  (and the non-Param macros' underlying exports), EndDialog, DefDlgProcA/W (WM_INITDIALOG return
  → focus; DM_GETDEFID/DM_SETDEFID; WM_CLOSE → IDCANCEL; WM_ERASEBKGND with COLOR_3DFACE; WM_CTLCOLOR*
  via the DLGPROC; DWLP_MSGRESULT handling; dialog procs return INT_PTR and are ms_abi),
  IsDialogMessageA/W (Tab/Shift+Tab with WS_TABSTOP, arrow keys within WS_GROUP groups, Enter →
  default button (DM_GETDEFID / BS_DEFPUSHBUTTON), Esc → IDCANCEL, mnemonics (Alt+letter or letter
  when DLGC_WANTCHARS not set) via WM_GETDLGCODE), GetNextDlgTabItem, GetNextDlgGroupItem,
  MapDialogRect, GetDialogBaseUnits (if F doesn't), modal loop with owner disabled
  (EnableWindow(owner, FALSE)) and re-enabled on EndDialog, WM_ENTERIDLE.
* MessageBoxA/W/ExA/ExW/IndirectA/W/MessageBoxTimeoutA/W: dialog window (managed DIALOG kind,
  owner disabled while shown) with system icon (IDI_ERROR/WARNING/INFORMATION/QUESTION via
  LoadIcon), wrapped text (DrawText DT_CALCRECT with max width ~ 2/3 screen), buttons per MB_ type
  (OK, OKCANCEL, ABORTRETRYIGNORE, YESNOCANCEL, YESNO, RETRYCANCEL, CANCELTRYCONTINUE, MB_HELP),
  default button (MB_DEFBUTTON1..4), Esc/Enter, Ctrl+C copies text, MB_TOPMOST/SETFOREGROUND,
  works with hWnd NULL, and when no display is available prints the message to stderr and returns
  the default button. MessageBeep → plat_bell.
* Common dialog helpers that belong to user32-ish UI and are small: none required here
  (comdlg32 is package I).

### H. controls + comctl32 — owns `runtime/controls/*.c`

* `aw_controls_register()` registers (W procs, global classes, CS_DBLCLKS/CS_PARENTDC where
  Windows has them, case-insensitive names): `Button`, `Static`, `Edit`, `ListBox`, `ComboBox`,
  `ComboLBox`, `ScrollBar`, `SysListView32`, `SysHeader32`, `msctls_progress32`,
  `msctls_trackbar32`, `msctls_statusbar32`, `msctls_updown32`, `SysTabControl32`,
  `tooltips_class32`, `SysTreeView32`, `ToolbarWindow32` (basic), `ReBarWindow32` (stub host),
  `SysLink` (basic), `SysDateTimePick32`/`SysMonthCal32` (stub text), `RichEdit20A`, `RichEdit20W`,
  `RICHEDIT50W`, `RichEdit` (multiline Edit behavior + EM_SETCHARFORMAT ignored, EM_STREAMIN/OUT
  of plain text, EM_EXSETSEL/EXGETSEL), `msctls_hotkey32` (stub).
* Controls are ordinary Win32 code against the public API (`__aw_user32_*`, `__aw_gdi32_*`),
  store state in cbWndExtra (pointer) and draw in WM_PAINT; they notify parents with WM_COMMAND/
  WM_NOTIFY and ask WM_CTLCOLOR* for colors/brushes; respect WM_SETFONT/WM_GETFONT (default
  DEFAULT_GUI_FONT), WM_ENABLE (gray text), WM_SETTEXT/GETTEXT, WM_GETDLGCODE, WM_SETFOCUS/
  KILLFOCUS (focus rects), WM_PRINTCLIENT, WM_STYLECHANGED, WM_SIZE. Visual style: Windows 10
  classic-themed look (flat 1px borders 0xADADAD/0x0078D7 hover for buttons/edits is fine).
* **Button**: BS_PUSHBUTTON/DEFPUSHBUTTON, CHECKBOX/AUTOCHECKBOX, 3STATE/AUTO3STATE, RADIOBUTTON/
  AUTORADIOBUTTON (group auto-uncheck), GROUPBOX, OWNERDRAW (WM_DRAWITEM), PUSHLIKE, BS_LEFT/
  RIGHT/CENTER/TOP/BOTTOM/VCENTER/MULTILINE, BS_ICON/BS_BITMAP (BM_SETIMAGE), BS_FLAT,
  BS_SPLITBUTTON optional; BM_GETCHECK/SETCHECK/GETSTATE/SETSTATE/SETSTYLE/CLICK/GETIMAGE/SETIMAGE/
  SETDONTCLICK; BN_CLICKED/DBLCLK/SETFOCUS/KILLFOCUS (BS_NOTIFY); mouse capture press/release;
  space key; hot-tracking; default-button frame.
* **Static**: SS_LEFT/CENTER/RIGHT/LEFTNOWORDWRAP/SIMPLE/NOPREFIX/ENDELLIPSIS/PATHELLIPSIS/
  WORDELLIPSIS/CENTERIMAGE/ICON/BITMAP/BLACKRECT/GRAYRECT/WHITERECT/*FRAME/ETCHEDHORZ/ETCHEDVERT/
  ETCHEDFRAME/SUNKEN/OWNERDRAW/NOTIFY; STM_SETICON/GETICON/SETIMAGE/GETIMAGE; WM_CTLCOLORSTATIC
  brush paints the background (todo.exe returns a white stock brush).
* **Edit** (single- and multi-line): ES_LEFT/CENTER/RIGHT, ES_MULTILINE, ES_AUTOHSCROLL/
  AUTOVSCROLL, ES_PASSWORD (EM_SETPASSWORDCHAR, default '•' or '*'), ES_READONLY, ES_NUMBER,
  ES_UPPERCASE/LOWERCASE, ES_WANTRETURN, ES_NOHIDESEL; word wrap for multiline without
  WS_HSCROLL/ES_AUTOHSCROLL; caret (CreateCaret/SetCaretPos), selection with mouse (drag,
  double-click word select) and Shift+keys, Home/End/Ctrl+arrows/PageUp/Down, Backspace/Delete/
  Ctrl+Backspace, clipboard Ctrl+C/X/V/A and WM_COPY/CUT/PASTE/CLEAR/UNDO (single undo buffer),
  context menu (Undo/Cut/Copy/Paste/Delete/Select All), scrolling (horizontal for single-line,
  WS_VSCROLL for multi-line using standard scroll bars), EM_GETSEL/SETSEL/REPLACESEL/LIMITTEXT/
  SETLIMITTEXT/GETLIMITTEXT/GETLINECOUNT/GETLINE/LINEINDEX/LINELENGTH/LINEFROMCHAR/GETFIRSTVISIBLELINE/
  LINESCROLL/SCROLLCARET/SCROLL/GETMODIFY/SETMODIFY/CANUNDO/UNDO/EMPTYUNDOBUFFER/SETREADONLY/
  GETRECT/SETRECT(NP)/SETMARGINS/GETMARGINS/POSFROMCHAR/CHARFROMPOS/SETTABSTOPS/FMTLINES/
  GETHANDLE/SETHANDLE (return a GlobalAlloc copy)/SETCUEBANNER/GETCUEBANNER (gray cue text when
  empty & unfocused)/SHOWBALLOONTIP (ignore); notifications EN_CHANGE (after WM_SETTEXT too, like
  Windows), EN_UPDATE, EN_SETFOCUS, EN_KILLFOCUS, EN_MAXTEXT, EN_HSCROLL/VSCROLL; WM_CHAR handles
  UTF-16 (incl. surrogates). Text stored UTF-16.
* **ListBox**: LBS_NOTIFY/SORT/MULTIPLESEL/EXTENDEDSEL/OWNERDRAWFIXED/VARIABLE/HASSTRINGS/
  NOINTEGRALHEIGHT/USETABSTOPS/WANTKEYBOARDINPUT/NOSEL/MULTICOLUMN (basic), all LB_* messages
  (ADDSTRING/INSERTSTRING/DELETESTRING/RESETCONTENT/GETCOUNT/GETTEXT/GETTEXTLEN/SETCURSEL/
  GETCURSEL/SETSEL/GETSEL/GETSELCOUNT/GETSELITEMS/SELITEMRANGE(EX)/SETITEMDATA/GETITEMDATA/
  FINDSTRING/FINDSTRINGEXACT/SELECTSTRING/SETTOPINDEX/GETTOPINDEX/GETITEMRECT/GETITEMHEIGHT/
  SETITEMHEIGHT/ITEMFROMPOINT/SETCARETINDEX/GETCARETINDEX/SETHORIZONTALEXTENT/INITSTORAGE/
  SETCOUNT/DIR/SETTABSTOPS/SETCOLUMNWIDTH/SETANCHORINDEX/GETANCHORINDEX), keyboard nav, type-ahead,
  vertical scroll bar, LBN_SELCHANGE/DBLCLK/SETFOCUS/KILLFOCUS/SELCANCEL, WM_VKEYTOITEM/
  WM_CHARTOITEM, owner-draw via WM_MEASUREITEM/WM_DRAWITEM/WM_DELETEITEM/WM_COMPAREITEM.
* **ComboBox**: CBS_SIMPLE/DROPDOWN/DROPDOWNLIST/SORT/AUTOHSCROLL/OWNERDRAW*/HASSTRINGS/
  NOINTEGRALHEIGHT/UPPERCASE/LOWERCASE; edit child for DROPDOWN/SIMPLE; the drop-down list is a
  WS_POPUP `ComboLBox` window (override-redirect via user32) positioned under the combo (above if
  no room), height for ~ the window's original CreateWindow height or 30 items, SetCapture tracking,
  closing on outside click/Esc/Enter/selection; all CB_* messages (ADDSTRING/INSERTSTRING/
  DELETESTRING/RESETCONTENT/GETCOUNT/GETCURSEL/SETCURSEL/GETLBTEXT/GETLBTEXTLEN/FINDSTRING(EXACT)/
  SELECTSTRING/SETITEMDATA/GETITEMDATA/SHOWDROPDOWN/GETDROPPEDSTATE/GETDROPPEDCONTROLRECT/
  SETEDITSEL/GETEDITSEL/LIMITTEXT/SETITEMHEIGHT/GETITEMHEIGHT/SETEXTENDEDUI/GETEXTENDEDUI/
  SETDROPPEDWIDTH/GETDROPPEDWIDTH/SETMINVISIBLE/GETMINVISIBLE/SETCUEBANNER/GETCOMBOBOXINFO/
  INITSTORAGE/DIR/SETTOPINDEX/GETTOPINDEX), keyboard (arrows change selection without dropping for
  DROPDOWNLIST, F4/Alt+Down toggles), type-ahead; notifications CBN_SELCHANGE/SELENDOK/SELENDCANCEL/
  DROPDOWN/CLOSEUP/EDITCHANGE/EDITUPDATE/SETFOCUS/KILLFOCUS/DBLCLK.
* **ScrollBar** control (SBS_HORZ/VERT/SIZEBOX/SIZEGRIP): SBM_* messages + WM_HSCROLL/WM_VSCROLL
  to the parent; share drawing/tracking code idea with user32's NC scroll bars (but separate code
  is fine).
* **SysHeader32**: HDM_INSERTITEMA/W, DELETEITEM, GETITEMA/W, SETITEMA/W, GETITEMCOUNT, LAYOUT,
  HITTEST, GETITEMRECT, ORDERTOINDEX, GETORDERARRAY/SETORDERARRAY, SETIMAGELIST; HDS_BUTTONS/HOTTRACK/
  FULLDRAG/NOSIZING; column resize by dragging the divider (HDN_BEGINTRACK/TRACK/ENDTRACK/
  ITEMCHANGING/ITEMCHANGED/DIVIDERDBLCLICK), click (HDN_ITEMCLICK), sort arrows (HDF_SORTUP/DOWN),
  owner-draw, NM_CUSTOMDRAW basic.
* **SysListView32** (most important for todo.exe): LVS_REPORT (header child + rows + columns),
  LVS_LIST/LVS_ICON/LVS_SMALLICON (simple grid layout), LVS_SINGLESEL, LVS_SHOWSELALWAYS,
  LVS_SORTASCENDING/DESCENDING, LVS_NOCOLUMNHEADER, LVS_NOSORTHEADER, LVS_OWNERDATA (virtual lists
  via LVN_GETDISPINFO), LVS_OWNERDRAWFIXED, LVS_EDITLABELS (optional); extended styles
  LVS_EX_FULLROWSELECT, LVS_EX_GRIDLINES, LVS_EX_CHECKBOXES, LVS_EX_HEADERDRAGDROP,
  LVS_EX_DOUBLEBUFFER, LVS_EX_LABELTIP, LVS_EX_INFOTIP (ignore), LVS_EX_TRACKSELECT (optional).
  Messages (A and W variants): LVM_INSERTCOLUMNA/W, DELETECOLUMN, GETCOLUMNA/W, SETCOLUMNA/W,
  GETCOLUMNWIDTH, SETCOLUMNWIDTH (incl. LVSCW_AUTOSIZE/_USEHEADER), GETCOLUMNORDERARRAY/
  SETCOLUMNORDERARRAY, GETHEADER, INSERTITEMA/W, DELETEITEM, DELETEALLITEMS (LVN_DELETEALLITEMS/
  DELETEITEM), GETITEMA/W, SETITEMA/W, SETITEMTEXTA/W, GETITEMTEXTA/W, GETITEMCOUNT, SETITEMCOUNT,
  GETNEXTITEM (LVNI_ALL/SELECTED/FOCUSED/ABOVE/BELOW/…), GETITEMSTATE, SETITEMSTATE (index -1 = all),
  GETSELECTEDCOUNT, GETSELECTIONMARK/SETSELECTIONMARK, FINDITEMA/W, SORTITEMS (comparator(lParam1,
  lParam2, lParamSort) — **ms_abi callback into the program**), SORTITEMSEX, ENSUREVISIBLE,
  GETTOPINDEX, GETCOUNTPERPAGE, GETITEMRECT (LVIR_BOUNDS/ICON/LABEL/SELECTBOUNDS), GETSUBITEMRECT,
  GETITEMPOSITION, HITTEST, SUBITEMHITTEST, SCROLL, REDRAWITEMS, UPDATE, ARRANGE, GETVIEWRECT,
  GETORIGIN, SETEXTENDEDLISTVIEWSTYLE (mask semantics) / GETEXTENDEDLISTVIEWSTYLE,
  SETIMAGELIST/GETIMAGELIST, SETBKCOLOR/GETBKCOLOR, SETTEXTCOLOR/GETTEXTCOLOR,
  SETTEXTBKCOLOR/GETTEXTBKCOLOR, GETSTRINGWIDTHA/W, SETVIEW/GETVIEW, SETHOTITEM, SETITEMPOSITION
  (ignore), GETEDITCONTROL (NULL), SETUNICODEFORMAT/GETUNICODEFORMAT, CCM_SETVERSION.
  Behavior: header columns resizable; rows with focus/selection (highlight COLOR_HIGHLIGHT, or
  light gray when unfocused + SHOWSELALWAYS), full-row selection, gridlines, text ellipsis per
  cell, subitem alignment from LVCFMT, click/ctrl-click/shift-click selection, keyboard nav
  (arrows, Home/End, PgUp/PgDn, Ctrl+A, Space for checkboxes, type-ahead), vertical/horizontal
  scrolling via standard scroll bars (WS_VSCROLL/WS_HSCROLL managed by the control) + mouse wheel,
  header scrolls horizontally with the list. Notifications to the parent (WM_NOTIFY with
  NMHDR/NMLISTVIEW/NMITEMACTIVATE/NMLVDISPINFO/NMLVKEYDOWN/NMLVCUSTOMDRAW in the A or W form decided
  by WM_NOTIFYFORMAT): LVN_ITEMCHANGING/ITEMCHANGED (uOldState/uNewState/uChanged=LVIF_STATE —
  fired for every state change, including programmatic ones, exactly like Windows since todo.exe
  relies on LVN_ITEMCHANGED with LVIS_SELECTED), LVN_INSERTITEM, LVN_DELETEITEM, LVN_COLUMNCLICK
  (iSubItem), LVN_KEYDOWN, LVN_GETDISPINFOA/W (LPSTR_TEXTCALLBACK, I_IMAGECALLBACK, owner data),
  LVN_ODCACHEHINT, LVN_ITEMACTIVATE, LVN_BEGINDRAG (optional), LVN_HOTTRACK, NM_CLICK, NM_DBLCLK,
  NM_RCLICK, NM_RDBLCLK, NM_RETURN, NM_SETFOCUS, NM_KILLFOCUS, NM_CUSTOMDRAW (CDDS_PREPAINT/
  ITEMPREPAINT/SUBITEM; honor CDRF_NOTIFYITEMDRAW/NOTIFYSUBITEMDRAW/NEWFONT/SKIPDEFAULT and
  clrText/clrTextBk). Struct layouts must match x64 Windows exactly (NMHDR 24 bytes; NMLISTVIEW:
  iItem +0x18, iSubItem +0x1c, uNewState +0x20, uOldState +0x24, uChanged +0x28, ptAction +0x2c,
  lParam +0x38; LVITEMA/W: mask 0, iItem 4, iSubItem 8, state 0xc, stateMask 0x10, pszText 0x18,
  cchTextMax 0x20, iImage 0x24, lParam 0x28, iIndent 0x30, …; LVCOLUMNA/W: mask, fmt, cx,
  pszText at 0x10, cchTextMax 0x18, iSubItem 0x1c, iImage, iOrder, cxMin, cxDefault, cxIdeal).
* **msctls_progress32** (PBM_SETRANGE/SETRANGE32/SETPOS/DELTAPOS/SETSTEP/STEPIT/GETPOS/GETRANGE/
  SETBARCOLOR/SETBKCOLOR/SETMARQUEE/SETSTATE, PBS_SMOOTH/VERTICAL/MARQUEE), **msctls_trackbar32**
  (TBM_* core set, TBS_HORZ/VERT/AUTOTICKS/NOTICKS/BOTH, WM_HSCROLL/VSCROLL notifications with TB_*
  codes, mouse drag + keys), **msctls_statusbar32** (SB_SETTEXTA/W, SB_GETTEXTA/W, SB_SETPARTS,
  SB_GETPARTS, SB_SETMINHEIGHT, SB_SIMPLE, SB_GETRECT, SBARS_SIZEGRIP; auto-position at the bottom of
  the parent on WM_SIZE), **msctls_updown32** (UDM_SETRANGE(32)/GETRANGE/SETPOS(32)/GETPOS(32)/
  SETBUDDY/GETBUDDY/SETACCEL/SETBASE; UDS_SETBUDDYINT/ALIGNRIGHT/ALIGNLEFT/ARROWKEYS/AUTOBUDDY/
  WRAP; UDN_DELTAPOS; buddy edit text sync), **SysTabControl32** (TCM_INSERTITEMA/W, DELETEITEM,
  DELETEALLITEMS, GETITEMCOUNT, GETCURSEL, SETCURSEL, GETCURFOCUS, SETCURFOCUS, GETITEMA/W,
  SETITEMA/W, ADJUSTRECT, GETITEMRECT, HITTEST, SETIMAGELIST, SETITEMSIZE, SETPADDING,
  GETROWCOUNT; TCN_SELCHANGING/SELCHANGE; single row with scroll arrows OK), **tooltips_class32**
  (accept TTM_ADDTOOL/DELTOOL/UPDATETIPTEXT/ACTIVATE/SETMAXTIPWIDTH/RELAYEVENT/TRACKACTIVATE…;
  showing real tooltips optional — a POPUP window after hover delay if time permits),
  **SysTreeView32** (TVM_INSERTITEMA/W, DELETEITEM, EXPAND, GETNEXTITEM (all TVGN_*), GETITEMA/W,
  SETITEMA/W, SELECTITEM, GETCOUNT, ENSUREVISIBLE, HITTEST, GETITEMRECT, SETIMAGELIST,
  SORTCHILDREN(CB), GETINDENT/SETINDENT, SETBKCOLOR/SETTEXTCOLOR, GETEDITCONTROL; TVS_HASBUTTONS/
  HASLINES/LINESATROOT/SHOWSELALWAYS/CHECKBOXES/FULLROWSELECT; TVN_SELCHANGINGA/W, SELCHANGEDA/W,
  ITEMEXPANDINGA/W, ITEMEXPANDEDA/W, GETDISPINFOA/W, DELETEITEMA/W, KEYDOWN, NM_CLICK/DBLCLK/
  RCLICK/CUSTOMDRAW; keyboard nav; scroll bars), **ToolbarWindow32** (TB_BUTTONSTRUCTSIZE,
  ADDBUTTONSA/W, INSERTBUTTONA/W, DELETEBUTTON, SETIMAGELIST, ADDBITMAP (IDB_STD_SMALL_COLOR etc.
  → blank images), AUTOSIZE, ENABLEBUTTON, CHECKBUTTON, PRESSBUTTON, HIDEBUTTON, ISBUTTON*,
  GETBUTTON, BUTTONCOUNT, COMMANDTOINDEX, SETBUTTONSIZE, SETBITMAPSIZE, GETITEMRECT, GETRECT,
  SETEXTENDEDSTYLE, SETMAXTEXTROWS; flat buttons with text; WM_COMMAND on click; TBN_DROPDOWN).
* comctl32 exports: InitCommonControls, InitCommonControlsEx, `_TrackMouseEvent` (forward to
  user32 TrackMouseEvent), CreateStatusWindowA/W, CreateToolbarEx, CreateUpDownControl,
  GetEffectiveClientRect, ShowHideMenuCtl, MenuHelp (no-op), DrawStatusTextA/W,
  SetWindowSubclass/GetWindowSubclass/RemoveWindowSubclass/DefSubclassProc (chained subclassing
  via a window property + wndproc replacement; callbacks ms_abi; also ordinals 410/412/413/415 —
  modmap maps them), ImageList_Create/Destroy/Add/AddMasked/AddIcon/ReplaceIcon/Replace/Remove/
  GetImageCount/SetImageCount/GetIconSize/SetIconSize/Draw/DrawEx/DrawIndirect/GetIcon/
  SetBkColor/GetBkColor/BeginDrag/EndDrag/DragEnter/DragLeave/DragMove/SetDragCursorImage/
  LoadImageA/W/Duplicate/Merge/GetImageInfo/SetOverlayImage/Copy/Write/Read(fail)
  (HIMAGELIST = pointer to an ARGB strip), TaskDialog/TaskDialogIndirect (map to a MessageBox-style
  dialog: main instruction + content + common buttons + custom buttons; return the button id),
  PropertySheetA/W (return -1 / not supported unless time permits), LoadIconMetric/LoadIconWithScaleDown
  (→ LoadImage), DPA_*/DSA_* (Create, Destroy, InsertPtr, DeletePtr, GetPtr, SetPtr, GetPtrCount,
  Sort, Search, EnumCallback, DestroyCallback, InsertItem, GetItemPtr, DeleteItem, DeleteAllItems),
  Str_SetPtrW, FlatSB_* (forward to standard scroll bar APIs), DllGetVersion (6.0).
* Tests (with ignore-unresolved until everything links): MinGW programs creating each control in a
  window under `xvfb-run -a`, scripted with `xdotool` (type into edits, click buttons, open the
  combo dropdown and pick an item, click/ctrl-click ListView rows, column-click sort, keyboard nav),
  asserting via notifications logged to stdout and `import` screenshots.

### I. misc DLLs — owns `ole32.c`, `oleaut32.c`, `ws2_32.c`, `winmm.c`, `version.c`, `comdlg32.c`, `shlwapi.c`, `misc_dlls.c`

* **ws2_32** (Windows layouts!): WSAStartup (WSADATA), WSACleanup, WSAGetLastError/WSASetLastError
  (errno → WSAE* 10000+ codes), socket, closesocket, bind, listen, accept, connect, send, recv,
  sendto, recvfrom, shutdown, select (Windows `fd_set` = `{u_int fd_count; SOCKET fd_array[64]}`;
  SOCKET is UINT_PTR; convert both ways, timeouts), __WSAFDIsSet, ioctlsocket (FIONBIO, FIONREAD),
  setsockopt/getsockopt (SOL_SOCKET 0xffff and option numbers differ from Linux: SO_REUSEADDR 4,
  SO_KEEPALIVE 8, SO_BROADCAST 0x20, SO_LINGER 0x80, SO_SNDBUF 0x1001, SO_RCVBUF 0x1002,
  SO_SNDTIMEO/RCVTIMEO 0x1005/0x1006 as DWORD ms, SO_ERROR 0x1007, SO_TYPE 0x1008; IPPROTO_TCP
  TCP_NODELAY 1; IPPROTO_IP options), getsockname, getpeername, gethostname, gethostbyname,
  gethostbyaddr (static per-thread Windows `hostent` layout), getservbyname/port,
  getprotobyname, getaddrinfo/GetAddrInfoW/freeaddrinfo/FreeAddrInfoW (Windows `addrinfo` field
  order: flags, family, socktype, protocol, size_t addrlen, char *canonname, sockaddr *addr, next;
  AF_INET6 = 23 on Windows!; convert sockaddr families), getnameinfo/GetNameInfoW, inet_addr,
  inet_ntoa, inet_pton/InetPtonW, inet_ntop/InetNtopW, htons/htonl/ntohs/ntohl, WSAIoctl (minimal),
  WSASocketA/W, WSASend/WSARecv (blocking, no overlapped), WSAEventSelect/WSACreateEvent/
  WSACloseEvent/WSAWaitForMultipleEvents/WSAEnumNetworkEvents (best effort with a helper thread or
  poll), WSAPoll (Windows WSAPOLLFD), WSAAddressToStringA/W, WSAStringToAddressA/W,
  WSADuplicateSocket (fail). Make SOCKET values Linux fds (they're < 2^31).
* **winmm**: timeGetTime, timeBeginPeriod/timeEndPeriod, timeGetDevCaps, timeSetEvent/timeKillEvent
  (thread-based callbacks, ms_abi), PlaySoundA/W and sndPlaySoundA/W (play WAV via `aplay`/
  `paplay` child process if present for SND_FILENAME/SND_RESOURCE with SND_ASYNC; otherwise TRUE),
  waveOut* (fail with MMSYSERR_NODRIVER), joyGetNumDevs (0), joyGetPos(Ex) (JOYERR_UNPLUGGED),
  mciSendStringA/W (error), midiOut* (fail).
* **version**: GetFileVersionInfoSizeA/W, GetFileVersionInfoA/W, VerQueryValueA/W — read the
  VS_VERSIONINFO resource of the *own image* (paths naming the running program or NULL module),
  fail with ERROR_RESOURCE_DATA_NOT_FOUND for others.
* **comdlg32**: GetOpenFileNameA/W, GetSaveFileNameA/W (run `zenity --file-selection` (or
  `kdialog`) when available, honoring filters/initial dir/title/OFN_ALLOWMULTISELECT/
  OFN_OVERWRITEPROMPT, converting results to Windows paths; else return FALSE with
  CommDlgExtendedError 0), ChooseColorA/W (`zenity --color-selection` or FALSE), ChooseFontA/W
  (FALSE), PrintDlgA/W/PrintDlgExA/W/PageSetupDlgA/W (FALSE / E_FAIL), FindTextA/W/ReplaceTextA/W
  (NULL), GetFileTitleA/W, CommDlgExtendedError.
* **ole32/oleaut32/combase**: CoInitialize(Ex), CoUninitialize, OleInitialize/OleUninitialize,
  CoCreateInstance(Ex) → REGDB_E_CLASSNOTREG (0x80040154), CoGetClassObject, CoTaskMemAlloc/
  Realloc/Free, CoCreateGuid, StringFromGUID2, StringFromCLSID, CLSIDFromString, IIDFromString,
  IsEqualGUID helpers, CoRegisterClassObject (store), CoRevokeClassObject, CoInitializeSecurity
  (S_OK), CoGetMalloc (IMalloc vtable with ms_abi methods), OleFlushClipboard/OleSetClipboard
  (E_NOTIMPL), RegisterDragDrop/RevokeDragDrop (S_OK no-op), DoDragDrop (E_NOTIMPL),
  CreateStreamOnHGlobal (minimal IStream), PropVariantClear; oleaut32: SysAllocString(Len),
  SysAllocStringByteLen, SysReAllocString(Len), SysFreeString, SysStringLen, SysStringByteLen
  (BSTR = length-prefixed UTF-16), VariantInit, VariantClear, VariantCopy, VariantChangeType
  (basic numeric/string), SafeArrayCreate/Destroy/AccessData/UnaccessData/GetLBound/GetUBound/
  GetElement/PutElement (1-D), VarBstrCmp, LoadTypeLib (fail), RoInitialize/RoUninitialize (S_OK).
* **shlwapi**: PathFileExistsA/W, PathIsDirectoryA/W, PathCombineA/W, PathAppendA/W,
  PathRemoveFileSpecA/W, PathFindFileNameA/W, PathFindExtensionA/W, PathRemoveExtensionA/W,
  PathRenameExtensionA/W, PathAddBackslashA/W, PathRemoveBackslashA/W, PathStripPathA/W,
  PathIsRelativeA/W, PathCanonicalizeA/W, PathQuoteSpacesA/W, PathUnquoteSpacesA/W,
  PathGetArgsA/W, PathIsFileSpecA/W, PathMatchSpecA/W, PathCompactPathExA/W, PathIsURLA/W,
  StrStrIA/W, StrStrA/W, StrChrA/W, StrRChrA/W, StrCmpIA/W, StrCmpNIA/W, StrCmpLogicalW, StrDupA/W,
  StrToIntA/W, StrToIntExA/W, StrTrimA/W, StrFormatByteSizeA/W/64, wnsprintfA/W, wvnsprintfA/W,
  SHGetValueA/W, SHSetValueA/W, SHDeleteValueA/W, SHDeleteKeyA/W, SHRegGetValueA/W (via advapi32),
  UrlEscapeA/W/UrlUnescapeA/W (basic), SHCreateStreamOnFileA/W/Ex (minimal IStream), IsOS (FALSE
  except OS_NT), AssocQueryStringA/W (fail), HashData, SHAutoComplete (S_OK).
* **misc stubs** (each in its namespace, returning the documented failure/no-op values):
  uxtheme (SetWindowTheme S_OK, IsThemeActive/IsAppThemed FALSE, OpenThemeData NULL,
  CloseThemeData, DrawThemeBackground E_FAIL, GetThemeColor E_FAIL, EnableThemeDialogTexture S_OK,
  BufferedPaintInit/UnInit S_OK, BeginBufferedPaint NULL…), dwmapi (DwmSetWindowAttribute/
  DwmGetWindowAttribute E_NOTIMPL… but DWMWA_* not needed, DwmIsCompositionEnabled → FALSE,
  DwmExtendFrameIntoClientArea S_OK, DwmFlush S_OK), imm32 (ImmGetContext NULL, ImmReleaseContext,
  ImmAssociateContext(Ex), ImmSetCompositionWindow, ImmGetCompositionStringA/W 0,
  ImmIsIME FALSE, ImmDisableIME TRUE), psapi/kernel32 K32* (GetModuleFileNameExA/W,
  GetModuleBaseNameA/W, EnumProcesses (own pid), EnumProcessModules (image), GetModuleInformation,
  GetProcessMemoryInfo), dbghelp (SymInitialize FALSE, SymCleanup, StackWalk64 FALSE,
  MiniDumpWriteDump FALSE, SymFromAddr FALSE, UnDecorateSymbolName copies), bcrypt
  (BCryptGenRandom via getrandom, BCryptOpen/CloseAlgorithmProvider), secur32/GetUserNameExA/W if
  not C's, userenv (GetUserProfileDirectoryA/W), setupapi (SetupDiGetClassDevs INVALID), hid/
  xinput1_3/1_4/9_1_0 (XInputGetState → ERROR_DEVICE_NOT_CONNECTED, XInputSetState, XInputEnable,
  XInputGetCapabilities), d3d9/d3d11/dxgi/opengl32 creation functions fail cleanly (NULL / error
  HRESULT) so programs fall back or report errors, wininet/winhttp open functions fail,
  netapi32 (NetApiBufferFree, NetGetJoinInformation fail), mpr (WNetGetConnection fail),
  powrprof (CallNtPowerInformation STATUS_NOT_IMPLEMENTED), wtsapi32 (WTSRegisterSessionNotification
  TRUE), crypt32 (CryptProtectData copy-through, CryptUnprotectData copy-through, CryptStringToBinary/
  CryptBinaryToString base64/hex), ntdll common exports (RtlGetVersion, RtlInitUnicodeString,
  NtQuerySystemInformation STATUS_NOT_IMPLEMENTED, NtQueryInformationProcess minimal,
  RtlNtStatusToDosError if not C's, RtlGetNtVersionNumbers, NtDelayExecution, RtlAllocateHeap/
  RtlFreeHeap → kernel32 heap, RtlCompareMemory, RtlComputeCrc32, wine_get_version absent!).

---

## 7. Integration phase (after A–I report back)

1. `make` must build `build/anywin` + `build/libanywin.o` warning-free. Fix link errors (duplicate
   symbols between packages: pick one owner; missing internal hooks).
2. `build/anywin --list todo.exe` → all imports `ok`.
3. Relink and run todo.exe headless first:
   `build/anywin todo.exe -o build/todo && xvfb-run -a -s '-screen 0 1280x1024x24' build/todo`
   with `ANYWIN_DEBUG=1`; capture screenshots (`import -window root shot.png`) and compare against
   expectations: window with labels, edits, combo with 1–5, buttons, ListView with 6 column headers.
4. Scripted acceptance (xdotool under Xvfb):
   * type a title + description, pick priority, click Add → row appears in the ListView with
     "Pending" and a timestamp; data file `~/.config/TodoApp/todos.dat` written.
   * select row → fields populated (LVN_ITEMCHANGED path); Edit/Save; Complete → status
     "Completed"; Delete → row gone; empty title → MessageBox "Please enter a title!" (dismiss with
     Enter); search box filters rows (EN_CHANGE); column header click sorts (LVM_SORTITEMS callback).
   * restart the program → todos reloaded from the data file.
   * close button → window hides (WM_CLOSE handler) and the tray icon remains; tray right-click →
     popup menu Show/Hide/Start with Windows/Exit; Show restores; "Start with Windows" toggles the
     registry value and `~/.config/autostart/TodoApp.desktop`; Exit quits. (Xvfb has no tray:
     test the tray path with a minimal tray host, e.g. `stalonetray`/`trayer` if installable, or on
     the real display after asking the user. Without a tray, the window must still be reachable:
     if `plat_tray_add` fails, Shell_NotifyIcon should still return TRUE; consider making WM_CLOSE
     behavior unchanged — it's the program's choice.)
   * second instance → activates the first (FindWindow + mutex path) and exits.
5. Run on the real display `:0` only after asking the user; take a screenshot to show them.
6. Generalization checks (each must relink and run correctly):
   * MinGW console: hello world, printf/scanf torture, file I/O, threads, C++ iostream + exceptions
     + RTTI + std::thread (`x86_64-w64-mingw32-g++ -static`), `-municode` wmain, ucrt-linked build.
   * MinGW GUI: a classic WinMain with menu bar + accelerators + dialog from a .rc resource
     (`x86_64-w64-mingw32-windres`), common controls showcase, GDI drawing program, a program using
     DIB sections + StretchDIBits animation with timers.
   * If possible, a real third-party MinGW/MSVC console exe (e.g. download a small open-source tool
     built for Windows — ask the user first) to see what's missing; iterate with `--list` and the
     "unimplemented function called" log.
7. Write `README.md` (usage, architecture, limits, env vars: ANYWIN_DEBUG, ANYWIN_STRICT,
   ANYWIN_LDFLAGS, ANYWIN_DRIVE_X, ANYWIN_FONT_DIR, CC) and `tests/run.sh` that builds all test
   exes with mingw, relinks and runs them (GUI ones under xvfb-run), reporting pass/fail.

## 8. Known risks / gotchas

* **%gs**: nothing in glibc touches it, but every thread created outside `aw_create_thread`
  (e.g. glibc-internal threads, `timer_create` SIGEV_THREAD, helper threads in our own runtime)
  has no TEB; runtime helper threads that call into program code must call `aw_teb_create()` first.
* **Signals** interrupt program code with `%gs` intact; handlers must not assume SysV-preserved
  registers in program frames.
* Calls between runtime (SysV) and program (ms_abi) code: only through `WINAPI`/`AWCALL`
  declarations. A missed `AWCALL` on a callback pointer = silent register corruption.
* Win64 varargs: callee reads the 4 register args from the home area — `__builtin_ms_va_start`
  handles this only in functions declared `WINAPI` and variadic.
* Stack: program code assumes ≥ 1 MB stack and uses `___chkstk_ms` probes; Linux main thread
  grows on demand; worker threads get 8 MB.
* Images whose preferred base overlaps the runtime are handled by moving the runtime
  (`-Ttext-segment`). PE files without `.reloc` are fine (we never relocate).
* MinGW pseudo-relocations write into `.text`/`.rdata` after VirtualProtect: VirtualProtect must
  really `mprotect` and VirtualQuery must report correct region bases/sizes for image pages.
* `long` is 32-bit in Windows structs/printf; `time_t` in MinGW/ucrt is 64-bit (`_time64`).
* Windows `errno` numbering differs above 34; signal numbers differ (SIGABRT 22, SIGBREAK 21).
* X11: never block forever waiting for a reply while events are queued; handle `Expose` by
  re-putting the backbuffer (no program repaint needed); ConfigureNotify from reparenting WMs
  reports parent-relative coordinates (use synthetic events or TranslateCoordinates).
* Fonts: Noto Sans has large line metrics (winAscent+winDescent ≈ 1.36 em) vs Segoe UI (1.33);
  acceptable. Prefer the OS/2 win metrics like Windows does.
