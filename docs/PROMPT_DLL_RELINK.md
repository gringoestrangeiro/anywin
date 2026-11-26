# Task: DLL relinking for anywin

Repo: `/media/gringo/jogos/projetos/anywin` (static PE -> Linux ELF relinker; tool in `tool/anywin.c`,
runtime in `runtime/*.c`, module registry/`LoadLibrary` in `runtime/kernel32.c`). Read `README.md` first.
Today an exe's own sibling DLLs (non-system ones) are not relinked; only system DLLs are emulated by the runtime.
Make anywin relink the DLLs an exe uses, into the same standalone ELF. General design, no per-program hacks.

## Requirements
- **Detection (at relink time):** start from the exe; for every import DLL, look in the exe's folder
  (case-insensitive, also `--dll-path DIR` extra dirs). Found and not a system DLL (kernel32, user32, ... = anything the
  runtime already emulates) -> relink it too, then recurse on *its* imports. Also scan exe/DLL data for
  `*.dll` string literals (ASCII + UTF-16) to catch `LoadLibrary("foo.dll")` targets, and bundle those if found.
- **Flag:** `--no-dlls` disables all of it (current behavior). `--list` shows each DLL and why it was included
  (import / string / `--bundle-all`).
- **Runtime:** all modules live in one ELF. `LoadLibrary*`, `GetModuleHandle*`, `GetProcAddress` (by name and ordinal),
  `FreeLibrary` resolve embedded modules first, then fake system modules. `DllMain` is called with
  PROCESS_ATTACH/DETACH and THREAD_ATTACH/DETACH in Windows order (dependencies first); base relocations, TLS
  callbacks, forwarded exports, delay-load imports and per-module SEH tables must work.
- **Undetectable dynamic loads** (path built at runtime, DLL not named in the binary): `--bundle-all` embeds every
  non-system `*.dll` in the exe's folder, and `--bundle FILE` embeds a named one. A `LoadLibrary` that still
  misses returns NULL/ERROR_MOD_NOT_FOUND(126) and, with `ANYWIN_STRICT=1`, logs the exact name so the user can add it.
  No real PE loader at runtime (keeps the project's "no loader" rule).
- Keep it small: no regressions in `make test`; add a test with a MinGW-built exe + DLL pair
  (import-linked, `LoadLibrary`-by-literal, and computed-name case with `--bundle`).

## Agent plan (user asked for this split)
Use **Haiku 5.5 subagents at high effort**, at most **2 to start**, each with a private build dir
(`make BUILD=build-a` / `build-b`), no `git commit`, token-efficient, final report <= 10 lines.
- **Agent A (tool side):** `tool/anywin.c` - DLL discovery (imports, recursion, string scan, `--dll-path`,
  `--bundle`, `--bundle-all`, `--no-dlls`, `--list` output), relocating/embedding multiple images, emitting a module table.
- **Agent B (runtime side):** embedded-module registry in `runtime/kernel32.c` (+ new `runtime/modules.c`),
  `GetProcAddress`/ordinals/forwarders, `DllMain` ordering, TLS callbacks, per-module SEH in `runtime/seh.c`,
  `FreeLibrary` refcounts, and the MinGW test pair in `tests/`.
Agree on the module-table format first (A defines it in a header, B consumes it). The orchestrator verifies,
then spawns more agents to fix whatever fails.
