# anywin

A static Windows → Linux relinker. It takes an x86-64 PE executable and produces a standalone
native Linux ELF that calls Linux implementations of the imported Win32 functions.
No Wine, no PE loader at runtime, no compatibility layer at runtime.

> **Status: Stage 1 works.** The first target, `todo.exe` (a MinGW-built Win32 GUI app), relinks
> and runs. The runtime grows one capability at a time, driven by example executables.

## Usage

```sh
make                                   # builds build/anywin and build/libanywin.o
build/anywin todo.exe -o build/todo    # relink
./build/todo                           # runs on your normal X11 display
build/anywin --list todo.exe           # show which imports are implemented
```

`xvfb-run` is only used by the tests (a throwaway virtual display so they never touch your
desktop); the relinked program is an ordinary X11 client.

## How it works

1. `anywin` reads the PE, resolves its imports against the runtime's export table and
   relinks the image into an ELF together with `libanywin.o`.
2. The runtime (`runtime/`) implements the Win32 subset in C. The Windows calling convention
   (`ms_abi`) is used at the boundary.
3. Graphics go through a hand-written X11 wire-protocol client and a small TrueType
   rasterizer, so the output has no X11 or font library dependencies.

## What is implemented (Stage 1)

| Area | Files | Notes |
|---|---|---|
| kernel32 | `kernel32.c`, `sync.c`, `util.c` | command line/env defaults, module handles, `GetProcAddress`, real `VirtualProtect`/`VirtualQuery`, critical sections, mutexes, TLS |
| msvcrt / UCRT | `msvcrt.c`, `printf.c` | startup, stdio (48-byte `FILE` layout), heap, strings, time, `atexit`, signals, one printf engine with Win64 argument slots; both msvcrt and UCRT startup names |
| X11 + fonts | `x11.c`, `font.c` | wire-protocol client (windows, images, input, clipboard, XEmbed tray), TrueType rasterizer (cmap 4/12, glyf, AA, glyph cache) |
| GDI | `surface.c`, `region.c`, `gdi*.c`, `drawtext.c` | DCs, brushes, pens, fonts, regions, text, basic primitives |
| user32 | `user32_*.c` | window classes, `CreateWindowExA`, message queue, timers, painting/update regions, input, focus/capture, non-client borders and scrollbars, icons/cursors |
| Menus / dialogs | `menu.c`, `menu_popup.c`, `msgbox.c` | popup menus, `TrackPopupMenu`, `MessageBox` |
| Controls | `controls/*.c` | Button, Static, Edit, ComboBox, SysListView32, SysHeader32 |
| advapi32 | `advapi32.c` | file-backed registry under `~/.anywin/registry/`; Run-key values also write `~/.config/autostart/*.desktop` |
| shell32 | `shell32.c` | `Shell_NotifyIcon` over the X11 system tray |
| SEH | `seh.c` | `__C_specific_handler`, unwind lookup, unhandled-exception filter, crash report with RIP/RVA |

`runtime/SCOPED_SRCS` lists the files that are built. Deferred files such as `ws2_32.c` stay on
disk but are not compiled.

## Results on `todo.exe`

Verified under Xvfb with `ANYWIN_STRICT=1`: clean relink (108 imports, 0 unimplemented), full
form and ListView render, add / select / edit / complete / delete / search / column sort, data
survives a restart, empty-title MessageBox, no unimplemented calls.

Not done or not tested yet:

* Second-instance detection (the named mutex and `FindWindow` are per-process).
* Tray clicks end to end (Xvfb has no tray manager).
* ListView type-ahead, rubber-band selection, column drag reorder; multiline Edit; undo.
* DIB sections, `BitBlt` from bitmaps, palettes, paths, C++ exceptions, networking, COM.

## Design rules

* Implement general Win32 mechanisms, never application-specific behavior (no checks on
  program name, window class/title, file name or control id). `tests/no_overfit.sh` greps for this.
* Scope grows by example executables; each new capability gets a regression test.

## Environment variables

| Variable | Purpose |
|---|---|
| `ANYWIN_DEBUG` | trace runtime activity |
| `ANYWIN_STRICT` | abort on calls to unimplemented functions |
| `ANYWIN_LDFLAGS` | extra linker flags used by `anywin` |
| `ANYWIN_DRIVE_C` | host directory mapped to `C:\` |
| `ANYWIN_FONT_DIR` | extra font directory |
| `CC` | C compiler used for build and relink |

## Requirements

Linux x86-64, gcc, GNU ld. For tests: `xvfb-run`, `xdotool`, ImageMagick `import`, `python3`,
and `x86_64-w64-mingw32-gcc` to build probe programs.

## Tests

```sh
make test      # tests/run.sh: hooks, imports, hello_console, hello_gui, todo smoke, todo P0
```

Package tests (run individually): `core_env`, `core_stdio`, `core_gui`, `core_gui_controls`,
`core_menu`, `core_scroll`, `core_registry`, `core_seh`, `core_tray`, `gdi_render`,
`region_unit`, `listview_unit`, `x11_font`. Tests that open windows always use `xvfb-run`.

## Known issues

* An open ComboBox drop-list only closes on the next in-app click (clicks outside every app
  window are not delivered); mouse coordinates are relative to the wrong window while another
  top-level holds capture.
* The XEmbed tray icon used to be mapped by the client, which crashed Cinnamon (Muffin) when
  the app exited; it is now left to the tray manager. Please report if you still see it.
