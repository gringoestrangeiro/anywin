#!/usr/bin/env python3 -I
"""anywin vs Wine benchmark. Usage: python3 -I tests/bench.py [runs]   (needs Xvfb, xdotool, wine)
Everything runs on a private Xvfb display with a throw-away HOME and WINEPREFIX."""
import os, sys, time, json, shutil, signal, statistics as st, subprocess as sp, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 7
TMP = tempfile.mkdtemp(prefix="awbench.")
DISP = ":97"
HOME = os.path.join(TMP, "home"); os.makedirs(HOME + "/.config")
PFX = os.path.join(TMP, "pfx")
BID = "awbench%d" % os.getpid()
ENV = dict(os.environ, DISPLAY=DISP, HOME=HOME, WINEPREFIX=PFX, WINEDEBUG="-all", BENCH_ID=BID,
           WINEDLLOVERRIDES="mscoree,mshtml=d", XDG_CONFIG_HOME=HOME + "/.config")
TODO_EXE = os.path.join(ROOT, "todo.exe")
CPU_EXE = os.path.join(ROOT, "build/bench_cpu.exe")
AW = os.path.join(ROOT, "build/anywin")
TICK = os.sysconf("SC_CLK_TCK")

def pids():
    out = []
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        try:
            if ("BENCH_ID=%s\0" % BID).encode() in open("/proc/%s/environ" % p, "rb").read(): out.append(int(p))
        except OSError: pass
    return out

def mem(ps):
    rss = pss = 0
    for p in ps:
        try:
            for l in open("/proc/%d/smaps_rollup" % p):
                if l.startswith("Rss:"): rss += int(l.split()[1])
                elif l.startswith("Pss:"): pss += int(l.split()[1])
        except OSError: pass
    return rss / 1024, pss / 1024

def cpu(ps):
    t = 0
    for p in ps:
        try:
            f = open("/proc/%d/stat" % p).read().rsplit(")", 1)[1].split()
            t += int(f[11]) + int(f[12])
        except OSError: pass
    return t / TICK

def have_window():
    r = sp.run(["xdotool", "search", "--onlyvisible", "--name", "Todo Application"], env=ENV, capture_output=True, text=True)
    return bool(r.stdout.strip())

def killall(wine):
    for p in pids():
        try: os.kill(p, signal.SIGKILL)
        except OSError: pass
    if wine: sp.run(["wineserver", "-k"], env=ENV, capture_output=True)
    time.sleep(0.5)

def gui_run(cmd, wine):
    killall(wine)
    shutil.rmtree(HOME + "/.config/TodoApp", ignore_errors=True)
    t0 = time.time()
    pr = sp.Popen(cmd, env=ENV, stdout=sp.DEVNULL, stderr=sp.DEVNULL, start_new_session=True)
    while not have_window():
        if time.time() - t0 > 60: raise SystemExit("window never appeared: %s" % cmd)
        time.sleep(0.01)
    startup = (time.time() - t0) * 1000
    time.sleep(3)
    ps = pids(); rss, pss = mem(ps); c0 = cpu(ps); time.sleep(5); c1 = cpu(pids())
    res = dict(startup_ms=startup, rss_mb=rss, pss_mb=pss, idle_cpu_pct=(c1 - c0) / 5 * 100, procs=len(ps))
    killall(wine)
    return res

def cmd_run(cmd, wine, parse=False):
    # output goes to a file, not a pipe: a Wine server started by this command would otherwise
    # inherit the pipe and keep it open after the program exits
    of = os.path.join(TMP, "out.txt")
    t0 = time.time()
    with open(of, "w") as f:
        sp.run(cmd, env=ENV, stdout=f, stderr=sp.DEVNULL, timeout=120)
    return (time.time() - t0) * 1000, open(of).read()

def med(rs, k): return st.median(r[k] for r in rs)

xv = sp.Popen(["Xvfb", DISP, "-screen", "0", "1280x1024x24"], stdout=sp.DEVNULL, stderr=sp.DEVNULL)
time.sleep(1.5)
report = {}
try:
    # relink
    t = []
    for _ in range(RUNS):
        t0 = time.time(); sp.run([AW, TODO_EXE, "-o", os.path.join(TMP, "todo")], capture_output=True); t.append((time.time() - t0) * 1000)
    report["relink_ms"] = st.median(t)
    report["anywin_todo_bytes"] = os.path.getsize(os.path.join(TMP, "todo"))
    sp.run([AW, CPU_EXE, "-o", os.path.join(TMP, "cpu")], capture_output=True)
    # wine prefix init
    t0 = time.time(); sp.run(["wineboot", "-i"], env=ENV, capture_output=True, timeout=300); sp.run(["wineserver", "-w"], env=ENV, timeout=120)
    report["wine_prefix_init_s"] = time.time() - t0
    report["wine_prefix_mb"] = int(sp.run(["du", "-sm", PFX], capture_output=True, text=True).stdout.split()[0])
    killall(True)
    # GUI
    A = [gui_run([os.path.join(TMP, "todo")], False) for _ in range(RUNS)]
    W = [gui_run(["wine", TODO_EXE], True) for _ in range(RUNS)]
    report["gui"] = {n: {k: med(rs, k) for k in rs[0]} for n, rs in (("anywin", A), ("wine", W))}
    # console startup (trivial program) + CPU benchmark
    # trivial console exe built here so it is always available
    hc = os.path.join(TMP, "hi.c"); open(hc, "w").write('#include <stdio.h>\nint main(void){puts("hi");return 0;}\n')
    sp.run(["x86_64-w64-mingw32-gcc", "-O2", "-o", os.path.join(TMP, "hi.exe"), hc], check=True)
    sp.run([AW, os.path.join(TMP, "hi.exe"), "-o", os.path.join(TMP, "hi")], capture_output=True)
    killall(True)
    a = [cmd_run([os.path.join(TMP, "hi")], False)[0] for _ in range(RUNS * 3)]
    killall(True)
    w_cold = []
    for _ in range(RUNS): killall(True); w_cold.append(cmd_run(["wine", os.path.join(TMP, "hi.exe")], True)[0])
    sp.Popen(["wineserver", "-p", "-f"], env=ENV, stdout=sp.DEVNULL, stderr=sp.DEVNULL, start_new_session=True); time.sleep(2)
    w_warm = [cmd_run(["wine", os.path.join(TMP, "hi.exe")], True)[0] for _ in range(RUNS * 3)]
    report["console_start_ms"] = dict(anywin=st.median(a), wine_cold=st.median(w_cold), wine_warm=st.median(w_warm))
    # CPU / CRT
    def cpu_bench(cmd):
        rs = []
        for _ in range(RUNS):
            w, out = cmd_run(cmd, True)
            d = {l.split()[0]: float(l.split()[1]) for l in out.splitlines() if "_ms" in l}
            d["wall_ms"] = w; rs.append(d)
        return {k: med(rs, k) for k in rs[0]}
    report["cpu"] = dict(anywin=cpu_bench([os.path.join(TMP, "cpu")]), wine=cpu_bench(["wine", CPU_EXE]))
    report["wine_version"] = sp.run(["wine", "--version"], capture_output=True, text=True).stdout.strip()
finally:
    killall(True); xv.kill()
    shutil.rmtree(TMP, ignore_errors=True)
print(json.dumps(report, indent=1))
