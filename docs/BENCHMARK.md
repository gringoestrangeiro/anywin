# Benchmark and optimization brief (anywin vs Wine)

Purpose: a reproducible baseline so an agent can optimize anywin and re-run the **same** benchmark
to prove the change. Baseline taken 2026-11-26 on commit `57c40c5`, Wine 11.11 (Staging),
12-core Linux 6.18 box, private Xvfb, medians of 7 runs.

## How to run

```sh
make                                              # build/anywin, build/libanywin.o
x86_64-w64-mingw32-gcc -O2 -o build/bench_cpu.exe tests/bench_cpu.c
python3 -I tests/bench.py 7 > build/bench_new.json   # ~5 minutes; prints JSON to stdout
```

Needs `Xvfb`, `xdotool`, `wine`, MinGW. It uses a private display (`:97`), a temporary `HOME` and
a fresh `WINEPREFIX`, never your desktop, and cleans up after itself. Use the same run count (7)
when comparing with the baseline. Always run on an otherwise idle machine.

What it measures (`tests/bench.py`):

* **relink_ms** - `anywin todo.exe -o ...`
* **gui.\*** - `todo.exe` start to window visible (`xdotool` polling), RSS/PSS summed over every
  process of the run (all Wine processes counted), idle CPU over 5 s, process count.
* **console_start_ms** - trivial console program, process start to exit (anywin, Wine cold with no
  wineserver, Wine warm with wineserver running).
* **cpu.\*** - `tests/bench_cpu.c` compiled once with MinGW and run unchanged on both:
  `int_loop_ms`, `malloc_ms`, `sort_ms`, `snprintf_ms`, `memcpy_ms`, `total_ms`.

## Baseline (raw JSON)

```json
{
 "relink_ms": 105.56,
 "anywin_todo_bytes": 3417856,
 "wine_prefix_init_s": 9.8,
 "wine_prefix_mb": 375,
 "gui": {
  "anywin": {
   "startup_ms": 15.14,
   "rss_mb": 7.69,
   "pss_mb": 7.68,
   "idle_cpu_pct": 0.0,
   "procs": 1
  },
  "wine": {
   "startup_ms": 557.55,
   "rss_mb": 450.95,
   "pss_mb": 194.53,
   "idle_cpu_pct": 1.4,
   "procs": 9
  }
 },
 "console_start_ms": {
  "anywin": 1.24,
  "wine_cold": 465.51,
  "wine_warm": 7.64
 },
 "cpu": {
  "anywin": {
   "int_loop_ms": 396.0,
   "malloc_ms": 59.0,
   "sort_ms": 325.0,
   "snprintf_ms": 836.0,
   "memcpy_ms": 137.0,
   "total_ms": 1756.0,
   "wall_ms": 1768.77
  },
  "wine": {
   "int_loop_ms": 388.0,
   "malloc_ms": 242.0,
   "sort_ms": 322.0,
   "snprintf_ms": 354.0,
   "memcpy_ms": 81.0,
   "total_ms": 1388.0,
   "wall_ms": 1417.02
  }
 },
 "wine_version": "wine-11.11 (Staging)"
}
```

## Where anywin is slower (optimization targets)

| Metric | anywin | Wine | Gap | Suspected cause (not profiled) |
|---|---|---|---|---|
| `snprintf_ms` | 836 | 354 | 2.4x | `runtime/printf.c` formats `%f/%e/%g` by calling glibc `snprintf` once per conversion, and the engine reads Win64 argument slots generically; the UCRT entry points (`__stdio_common_vsnprintf_s`) add a wrapper layer |
| `memcpy_ms` | 137 | 81 | 1.7x | the exported `memcpy` goes through the Windows-calling-convention (`ms_abi`) wrapper; static glibc may also pick a slower `memcpy` variant than Wine's dynamic glibc |

Everything else is already equal or better (startup, memory, `malloc`). Do not regress those.

## Task for an optimization agent

1. Profile first (`perf record` / `perf stat` on `build/bench_cpu`, relinked from
   `build/bench_cpu.exe`). Confirm or replace the suspected causes above before changing code.
2. Optimize in the owning files only (`runtime/printf.c`, `runtime/msvcrt.c`; check `runtime/aw.h`
   for the `API(...)` wrapper cost). Keep general Windows semantics; no benchmark-specific code
   (`tests/no_overfit.sh` must stay green).
3. Correctness gate: `make && tests/run.sh build` must still pass (7/7), plus `tests/core_stdio.sh`.
   Output of `printf`-family functions must be byte-identical to before for the formats covered
   in `tests/core_stdio_probe.c`.
4. Re-run `tests/bench.py 7`, then compare `cpu.anywin.*` and `gui.anywin.*` against the baseline
   above. Report a before/after table. A change counts only if the improvement is larger than the
   run-to-run noise (re-run twice).
5. Update the benchmark table in `README.md` and this file's baseline only with numbers from a
   real run, and note the commit hash.

## Known limits of this benchmark

* One machine; GUI startup is polled every few ms with `xdotool`, so it is accurate to about
  10-20 ms.
* Wine runs start a fresh wineserver each time (disk cache warm).
* One small GUI program and one synthetic CRT test. It says nothing about large applications,
  GDI-heavy drawing or Wine's much wider API coverage. A GUI drawing/scrolling benchmark (for
  example filling and scrolling a large ListView) is a good follow-up.
