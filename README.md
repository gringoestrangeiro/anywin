# anywin

A static Windows → Linux relinker. It takes an x86-64 PE executable and produces a standalone
native Linux ELF that calls Linux implementations of the imported Win32 functions.
No Wine, no PE loader at runtime, no compatibility layer at runtime.

> **Status: early / work in progress.** The first target is `todo.exe` (a MinGW-built Win32 GUI
> app). The runtime grows one capability at a time, driven by example executables.

## Usage

```sh
make                                   # builds build/anywin and build/libanywin.o
build/anywin todo.exe -o build/todo    # relink
xvfb-run -a -s '-screen 0 1280x1024x24' build/todo
build/anywin --list todo.exe           # show which imports are implemented
```

## How it works

1. `anywin` reads the PE, resolves its imports against the runtime's export table and
   relinks the image into an ELF together with `libanywin.o`.
2. The runtime (`runtime/`) implements the Win32 subset in C: kernel32, msvcrt, user32,
   gdi32, advapi32 (file-backed registry), shell32 (tray), common controls, and a minimal
   SEH. Windows calling convention (`ms_abi`) is used at the boundary.
3. Graphics go through a hand-written X11 wire-protocol client and a small TrueType
   rasterizer, so the output has no X11 or font library dependencies.

## Design rules

* Implement general Win32 mechanisms, never application-specific behavior (no checks on
  program name, window class/title, file name or control id).
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

Linux x86-64, gcc, GNU ld. For tests: `xvfb-run`, `xdotool`, ImageMagick `import`, and
`x86_64-w64-mingw32-gcc` to build probe programs.

## Tests

```sh
make test      # tests/run.sh
```
