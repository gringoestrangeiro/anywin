# Benchmark and optimization brief (anywin vs Wine)

Purpose: a reproducible baseline so an agent can optimize anywin and re-run the **same** benchmark
to prove the change. Baseline updated 2026-11-26 on commit `75294b7` (after the printf/memcpy
optimization), Wine 11.11 (Staging), 12-core Linux 6.18 box, private Xvfb, medians of 7 runs. The
original pre-optimization baseline was commit `57c40c5`.

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

## Baseline (raw JSON, commit `75294b7`)

```json
{
 "relink_ms": 94.72,
 "anywin_todo_bytes": 3438600,
 "wine_prefix_init_s": 9.68,
 "wine_prefix_mb": 375,
 "gui": {
  "anywin": {
   "startup_ms": 15.00,
   "rss_mb": 7.68,
   "pss_mb": 7.68,
   "idle_cpu_pct": 0.0,
   "procs": 1
  },
  "wine": {
   "startup_ms": 548.18,
   "rss_mb": 452.01,
   "pss_mb": 201.99,
   "idle_cpu_pct": 1.2,
   "procs": 9
  }
 },
 "console_start_ms": {
  "anywin": 1.22,
  "wine_cold": 414.82,
  "wine_warm": 7.61
 },
 "cpu": {
  "anywin": {
   "int_loop_ms": 391.0,
   "malloc_ms": 57.0,
   "sort_ms": 315.0,
   "snprintf_ms": 167.0,
   "memcpy_ms": 86.0,
   "total_ms": 1022.0,
   "wall_ms": 1065.92
  },
  "wine": {
   "int_loop_ms": 387.0,
   "malloc_ms": 225.0,
   "sort_ms": 319.0,
   "snprintf_ms": 338.0,
   "memcpy_ms": 81.0,
   "total_ms": 1351.0,
   "wall_ms": 1367.77
  }
 },
 "wine_version": "wine-11.11 (Staging)"
}
```

Two earlier 7-run series confirmed the change is well above run-to-run noise: CPU totals 1023 and
1027 ms vs Wine's 1352 and 1348 ms, `snprintf` 173 ms both times vs 338 ms, `memcpy` 81 and 87 ms
vs 81 ms.

## Before / after

| Metric | before (`57c40c5`) | after (`75294b7`) | Wine (after run) | Change |
|---|---|---|---|---|
| `snprintf_ms` | 836 | **167** | 338 | 5.0x faster |
| `memcpy_ms` | 137 | **86** | 81 | 1.6x faster, at parity |
| `cpu.total_ms` | 1756 | **1022** | 1351 | 24% faster than Wine |
| `int_loop_ms` | 396 | 391 | 387 | unchanged |
| `malloc_ms` | 59 | 57 | 225 | unchanged |
| `sort_ms` | 325 | 315 | 319 | unchanged |
| `gui.anywin.startup_ms` | 15.14 | 15.00 | 548 | unchanged |
| `gui.anywin.rss_mb` | 7.69 | 7.68 | 452 | unchanged |

## Profile findings

A probe that ran each conversion 1.5M times isolated the cost (anywin, before the change):

| Case | ms | Note |
|---|---|---|
| full benchmark format `"item %d: %s %5.2f 0x%x"` | 792 | |
| same without the float | 133 | int + string + hex + engine |
| `%5.2f` alone | 646 | 81% of the full format |
| literal only | 28 | buffer + malloc overhead |
| native glibc `%5.2f`, one pass | 233 | reference |

The float path was two glibc `snprintf` passes (sizing with `NULL,0`, then formatting) plus a
`malloc`/`free` per conversion, on top of the engine's own heap buffer. The UCRT wrapper and the
generic Win64 argument slots were not the bottleneck. For `memcpy`, the ms_abi wrapper cost was
about 1-2 ms of 137 ms; the gap was glibc's chosen ifunc variant, which is slow for 0.5-2 MiB
copies on this machine.

## What changed

* `runtime/printf.c`
  * `%f` is formatted exactly with 128-bit integer arithmetic (precision 0-9, scaled value that
    fits in 64 bits, round-half-to-even), no glibc call and no allocation in the common case.
  * Other float conversions (`%e`/`%g`/`%a`, precision > 9, huge or overflowing values) still use
    glibc, but with one pass into a 512-byte stack buffer; only oversized results spill to the
    heap.
  * Bounded formats (`snprintf` and friends) render into a caller-provided stack buffer and only
    fall back to the heap when the result does not fit; the 256-byte heap allocation per call is
    gone.
  * Format construction for the fallback no longer calls `snprintf` just to print the precision.
* `runtime/msvcrt.c`: the exported `memcpy` uses an AVX2 copy loop for 0.5-2 MiB requests (CPUID
  checked, glibc elsewhere), which matches Wine's throughput there instead of glibc's slower
  ifunc variant.
* `tests/core_stdio.sh`: the test still expected `__C_specific_handler` to be unimplemented, but
  `seh.c` now implements it; the check now requires zero unimplemented imports.

## Correctness evidence

* `make && tests/run.sh build`: 7/7 pass.
* `tests/core_stdio.sh build`: pass, including the byte-for-byte expected stdout.
* Differential harness: 2,722,585 random/targeted `%f`, `%e` and `%g` conversions compared with
  glibc `snprintf`, 0 mismatches (width/flags/precision 0-17, subnormals, ties, inf/nan).
* `memcpy` probe over 22 sizes from 0 to 5 MiB and 25 source/destination alignments: 0 failures.
* Other package tests pass; `core_seh` has two pre-existing "process ends by SIGSEGV" check
  failures that also occur on `57c40c5` and are unrelated.

## Known limits of this benchmark

* One machine; GUI startup is polled every few ms with `xdotool`, so it is accurate to about
  10-20 ms.
* Wine runs start a fresh wineserver each time (disk cache warm).
* One small GUI program and one synthetic CRT test. It says nothing about large applications,
  GDI-heavy drawing or Wine's much wider API coverage. A GUI drawing/scrolling benchmark (for
  example filling and scrolling a large ListView) is a good follow-up.
