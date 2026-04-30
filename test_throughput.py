#!/usr/bin/env python3
"""
NFD Throughput Benchmark — Baseline vs ORAM
============================================
Measures how total elapsed time grows with the number of operations.

  X axis: number of sequential Interest→Data round-trips
  Y axis: total elapsed time (seconds)
  Two lines: Baseline NFD vs ORAM NFD

Each "operation" = one complete Interest→Data round-trip:
  ndnpeek → NFD → ndnputchunks → NFD → ndnpeek

Two scenarios are measured:
  CS-HIT   — same content fetched repeatedly (ORAM read path)
  CS-MISS  — unique name every request (ORAM write path, worst case)

Usage:
  .venv/bin/python3 test_throughput.py [--no-plot] [--csv]

Options:
  --no-plot   Print table only, skip matplotlib graph
  --csv       Also write results to throughput_results.csv
"""

import os, sys, re, time, signal, subprocess, socket, threading
from typing import List, Optional, Tuple

# ── paths ─────────────────────────────────────────────────────────────────────

ORAM_BIN     = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "build", "bin", "nfd")
BASELINE_BIN = "/home/w4lker19/Projects/NFD/build/bin/nfd"
NFD_SOCK     = "/tmp/nfd_run/nfd.sock"
NFD_CONF     = "/tmp/nfd_test.conf"
NFD_LOG      = "/tmp/nfd_throughput.log"

# Operation counts to benchmark (X axis)
# Large range so the ORAM overhead gap is clearly visible on the graph.
OP_COUNTS = [1, 10, 50, 100, 250, 500, 1000, 2000, 5000, 10000, 20000]

# ── helpers ───────────────────────────────────────────────────────────────────

def run(cmd, timeout=15) -> Tuple[str, int]:
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True,
                           text=True, timeout=timeout)
        return r.stdout.strip(), r.returncode
    except subprocess.TimeoutExpired:
        return "", -1

def tool_exists(name):
    _, rc = run(f"which {name} 2>/dev/null")
    return rc == 0

def setup_conf():
    os.makedirs(os.path.dirname(NFD_SOCK), exist_ok=True)
    with open("/usr/local/etc/ndn/nfd.conf") as f:
        text = f.read()
    text = text.replace("path /run/nfd/nfd.sock", f"path {NFD_SOCK}")
    with open(NFD_CONF, "w") as f:
        f.write(text)
    os.environ["NDN_CLIENT_TRANSPORT"] = f"unix://{NFD_SOCK}"

def nfd_running():
    out, _ = run("nfdc status 2>/dev/null | head -3")
    return "version" in out

def kill_nfd():
    run("pkill -x nfd 2>/dev/null")
    for _ in range(14):
        time.sleep(0.5)
        if not nfd_running():
            return

def start_nfd(binary: str, label: str) -> Optional[subprocess.Popen]:
    if os.path.exists(NFD_SOCK):
        os.remove(NFD_SOCK)
    env = os.environ.copy()
    # Silence all logs — we only want timing numbers
    env["NDN_LOG"] = "nfd.*=NONE"
    env["NDN_CLIENT_TRANSPORT"] = f"unix://{NFD_SOCK}"
    log_fd = open(NFD_LOG, "w")
    proc = subprocess.Popen(
        [binary, "--config", NFD_CONF],
        stdout=log_fd, stderr=log_fd, env=env,
        preexec_fn=os.setsid,
    )
    for _ in range(20):
        time.sleep(0.5)
        if nfd_running():
            print(f"    [{label}] started (PID={proc.pid})")
            return proc
    print(f"    ERROR: {label} did not start")
    return None

def stop_nfd(proc):
    if proc is None:
        return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception:
        pass
    time.sleep(0.8)

# ── throughput measurement ────────────────────────────────────────────────────

def measure_cs_hit(n: int) -> float:
    """
    Time N sequential fetches of the SAME content name.
    After the first request, all hits come from the CS (ORAM read path).
    Returns total elapsed time in seconds.
    """
    prod = subprocess.Popen(
        'echo "throughput_hit" | ndnputchunks /t/tp/hit',
        shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    time.sleep(0.4)

    t0 = time.time()
    for _ in range(n):
        run("ndnpeek --prefix /t/tp/hit > /dev/null 2>&1", timeout=10)
    elapsed = time.time() - t0

    prod.terminate(); prod.wait()
    return elapsed

def measure_cs_miss(n: int) -> float:
    """
    Time N sequential fetches where EVERY request is a cache miss.
    Achieved by disabling CS admit+serve so data is never cached — every
    Interest must travel PIT insert → forward → producer → PIT erase.
    This exercises the full ORAM write path on every operation.
    Returns total elapsed time in seconds.
    """
    # Prevent CS from caching anything → guaranteed miss on every request
    run("nfdc cs config admit off serve off 2>/dev/null")
    time.sleep(0.1)

    prod = subprocess.Popen(
        'echo "throughput_miss" | ndnputchunks /t/tp/miss',
        shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    time.sleep(0.4)

    t0 = time.time()
    for _ in range(n):
        run("ndnpeek --prefix /t/tp/miss > /dev/null 2>&1", timeout=10)
    elapsed = time.time() - t0

    prod.terminate(); prod.wait()

    # Restore CS to normal
    run("nfdc cs config admit on serve on 2>/dev/null")
    time.sleep(0.1)
    return elapsed

def run_suite(binary: str, label: str) -> Tuple[List[float], List[float]]:
    """
    Run both CS-HIT and CS-MISS benchmarks for all OP_COUNTS.
    Returns (hit_times, miss_times) in seconds.
    """
    print(f"\n  ── {label}")
    kill_nfd()
    time.sleep(0.3)
    proc = start_nfd(binary, label)
    if proc is None:
        return [], []

    hit_times  = []
    miss_times = []

    print(f"    {'N ops':>8}  {'CS-HIT (s)':>12}  {'CS-MISS (s)':>12}  {'HIT ops/s':>10}  {'MISS ops/s':>11}")
    print(f"    {'─'*8}  {'─'*12}  {'─'*12}  {'─'*10}  {'─'*11}")
    for idx, n in enumerate(OP_COUNTS):
        print(f"    {n:>8}  (measuring...)", end="\r", flush=True)
        t_hit  = measure_cs_hit(n)
        t_miss = measure_cs_miss(n)
        hit_times.append(t_hit)
        miss_times.append(t_miss)
        print(f"    {n:>8}  {t_hit:>12.3f}  {t_miss:>12.3f}  "
              f"{n/t_hit:>10.1f}  {n/t_miss:>11.1f}")

    stop_nfd(proc)
    return hit_times, miss_times

# ── CSV export ────────────────────────────────────────────────────────────────

def write_csv(base_hit, base_miss, oram_hit, oram_miss):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "throughput_results.csv")
    with open(path, "w") as f:
        f.write("n_ops,baseline_hit_s,baseline_miss_s,oram_hit_s,oram_miss_s\n")
        for i, n in enumerate(OP_COUNTS):
            f.write(f"{n},{base_hit[i]:.4f},{base_miss[i]:.4f},"
                    f"{oram_hit[i]:.4f},{oram_miss[i]:.4f}\n")
    print(f"\n  CSV saved: {path}")

# ── matplotlib graph ──────────────────────────────────────────────────────────

def plot(base_hit, base_miss, oram_hit, oram_miss):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.gridspec as gridspec
    import numpy as np

    X  = np.array(OP_COUNTS)
    BH = np.array(base_hit)
    OH = np.array(oram_hit)
    BM = np.array(base_miss)
    OM = np.array(oram_miss)

    C_BASE   = "#1D4ED8"   # blue
    C_ORAM   = "#B91C1C"   # red
    C_FILL_H = "#BFDBFE"   # light blue fill
    C_FILL_M = "#FECACA"   # light red fill

    fig = plt.figure(figsize=(16, 10))
    fig.patch.set_facecolor("#F9FAFB")

    gs = gridspec.GridSpec(2, 2, hspace=0.42, wspace=0.32,
                           left=0.07, right=0.97, top=0.88, bottom=0.08)

    # ── top-left: CS-HIT total time ──────────────────────────────────────────
    ax1 = fig.add_subplot(gs[0, 0])
    ax1.set_facecolor("#F9FAFB")
    ax1.fill_between(X, BH, OH, alpha=0.30, color=C_FILL_M, label="ORAM overhead")
    ax1.plot(X, BH, color=C_BASE, lw=2.4, marker="o", ms=5,
             label="Baseline NFD (no ORAM)")
    ax1.plot(X, OH, color=C_ORAM, lw=2.4, marker="s", ms=5,
             label="ORAM NFD (PathORAM CS+PIT)")
    pct = (OH[-1] - BH[-1]) / BH[-1] * 100
    ax1.annotate(f"+{pct:.1f}% at N={X[-1]:,}",
                 xy=(X[-1], OH[-1]), xytext=(X[-4], OH[-1]*0.78),
                 fontsize=9, color=C_ORAM, fontweight="bold",
                 arrowprops=dict(arrowstyle="->", color=C_ORAM, lw=1.2))
    ax1.set_title("CS-HIT  —  same content fetched repeatedly\n"
                  "(ORAM read path: O(log N) bucket reads per lookup)",
                  fontsize=10, pad=6)
    ax1.set_xlabel("Number of operations", fontsize=10)
    ax1.set_ylabel("Total elapsed time (s)", fontsize=10)
    ax1.set_xlim(0, X[-1] * 1.05); ax1.set_ylim(bottom=0)
    ax1.legend(fontsize=8.5, loc="upper left", framealpha=0.7)
    ax1.grid(True, alpha=0.30, linestyle="--")
    ax1.spines[["top","right"]].set_visible(False)

    # ── top-right: CS-MISS total time ────────────────────────────────────────
    ax2 = fig.add_subplot(gs[0, 1])
    ax2.set_facecolor("#F9FAFB")
    ax2.fill_between(X, BM, OM, alpha=0.30, color=C_FILL_M, label="ORAM overhead")
    ax2.plot(X, BM, color=C_BASE, lw=2.4, marker="o", ms=5,
             label="Baseline NFD (no ORAM)")
    ax2.plot(X, OM, color=C_ORAM, lw=2.4, marker="s", ms=5,
             label="ORAM NFD (PathORAM CS+PIT)")
    pct2 = (OM[-1] - BM[-1]) / BM[-1] * 100
    ax2.annotate(f"+{pct2:.1f}% at N={X[-1]:,}",
                 xy=(X[-1], OM[-1]), xytext=(X[-4], OM[-1]*0.78),
                 fontsize=9, color=C_ORAM, fontweight="bold",
                 arrowprops=dict(arrowstyle="->", color=C_ORAM, lw=1.2))
    ax2.set_title("CS-MISS  —  every request goes to producer\n"
                  "(CS disabled: ORAM PIT insert+erase per operation)",
                  fontsize=10, pad=6)
    ax2.set_xlabel("Number of operations", fontsize=10)
    ax2.set_ylabel("Total elapsed time (s)", fontsize=10)
    ax2.set_xlim(0, X[-1] * 1.05); ax2.set_ylim(bottom=0)
    ax2.legend(fontsize=8.5, loc="upper left", framealpha=0.7)
    ax2.grid(True, alpha=0.30, linestyle="--")
    ax2.spines[["top","right"]].set_visible(False)

    # ── bottom-left: throughput (ops/s) ──────────────────────────────────────
    ax3 = fig.add_subplot(gs[1, 0])
    ax3.set_facecolor("#F9FAFB")
    ax3.plot(X, X/BH, color=C_BASE, lw=2.4, marker="o", ms=5,
             label="Baseline CS-HIT")
    ax3.plot(X, X/OH, color=C_ORAM, lw=2.4, marker="s", ms=5,
             label="ORAM CS-HIT")
    ax3.plot(X, X/BM, color=C_BASE, lw=2.0, marker="o", ms=4,
             linestyle="--", label="Baseline CS-MISS")
    ax3.plot(X, X/OM, color=C_ORAM, lw=2.0, marker="s", ms=4,
             linestyle="--", label="ORAM CS-MISS")
    ax3.set_title("Throughput  (operations per second)",
                  fontsize=10, pad=6)
    ax3.set_xlabel("Number of operations", fontsize=10)
    ax3.set_ylabel("ops / second", fontsize=10)
    ax3.set_xlim(0, X[-1] * 1.05)
    ax3.set_ylim(bottom=0)
    ax3.legend(fontsize=8, loc="lower right", framealpha=0.7)
    ax3.grid(True, alpha=0.30, linestyle="--")
    ax3.spines[["top","right"]].set_visible(False)

    # ── bottom-right: absolute overhead (seconds) ─────────────────────────────
    ax4 = fig.add_subplot(gs[1, 1])
    ax4.set_facecolor("#F9FAFB")
    delta_hit  = OH - BH
    delta_miss = OM - BM
    ax4.axhline(0, color="gray", lw=0.8, linestyle=":")
    ax4.bar(X - 60,  delta_hit,  width=100, color=C_ORAM,  alpha=0.80,
            label="CS-HIT overhead")
    ax4.bar(X + 60, delta_miss, width=100, color="#7C3AED", alpha=0.80,
            label="CS-MISS overhead")
    ax4.set_title("ORAM Absolute Overhead  (ORAM time − Baseline time)",
                  fontsize=10, pad=6)
    ax4.set_xlabel("Number of operations", fontsize=10)
    ax4.set_ylabel("Extra time (seconds)", fontsize=10)
    ax4.set_xlim(0, X[-1] * 1.10)
    ax4.legend(fontsize=8.5, loc="upper left", framealpha=0.7)
    ax4.grid(True, alpha=0.30, linestyle="--", axis="y")
    ax4.spines[["top","right"]].set_visible(False)

    # ── master title ──────────────────────────────────────────────────────────
    fig.text(0.5, 0.965,
             "NFD Throughput Benchmark — Baseline vs PathORAM",
             ha="center", fontsize=14, fontweight="bold")
    fig.text(0.5, 0.940,
             "Each operation = one complete Interest → NFD → Data round-trip  "
             f"(N up to {X[-1]:,} sequential ops)",
             ha="center", fontsize=10, color="#4B5563")

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "throughput_graph.png")
    fig.savefig(out, dpi=150, bbox_inches="tight", facecolor=fig.get_facecolor())
    print(f"\n  Graph saved: {out}")

# ── summary table ─────────────────────────────────────────────────────────────

def print_summary(base_hit, base_miss, oram_hit, oram_miss):
    print("\n" + "═"*68)
    print("  SUMMARY — throughput at maximum N =", OP_COUNTS[-1], "operations")
    print("═"*68)
    scenarios = [
        ("CS-HIT  (same content)", base_hit, oram_hit),
        ("CS-MISS (unique names)", base_miss, oram_miss),
    ]
    for label, b, o in scenarios:
        if not b or not o:
            continue
        b_tot, o_tot = b[-1], o[-1]
        delta = o_tot - b_tot
        pct   = delta / b_tot * 100
        b_ops = OP_COUNTS[-1] / b_tot
        o_ops = OP_COUNTS[-1] / o_tot
        print(f"\n  {label}")
        print(f"    Baseline : {b_tot:.2f} s  ({b_ops:.1f} ops/s)")
        print(f"    ORAM     : {o_tot:.2f} s  ({o_ops:.1f} ops/s)")
        print(f"    Overhead : +{delta:.2f} s  (+{pct:.1f}%)")
    print()

# ── main ──────────────────────────────────────────────────────────────────────

def main():
    args = set(sys.argv[1:])
    no_plot = "--no-plot" in args
    do_csv  = "--csv"     in args

    print("═"*64)
    print("  NFD Throughput Benchmark — Baseline vs ORAM")
    print("═"*64)
    print(f"  Baseline : {BASELINE_BIN}")
    print(f"  ORAM     : {ORAM_BIN}")
    print(f"  Op counts: {OP_COUNTS}")
    print(f"  Scenarios: CS-HIT (same name) | CS-MISS (unique name)")

    for t in ["nfdc", "ndnpeek", "ndnputchunks"]:
        if not tool_exists(t):
            print(f"ERROR: {t} not found"); sys.exit(1)

    setup_conf()

    base_hit, base_miss = run_suite(BASELINE_BIN, "Baseline NFD (no ORAM)")
    oram_hit, oram_miss = run_suite(ORAM_BIN,     "ORAM NFD (PathORAM)")

    print_summary(base_hit, base_miss, oram_hit, oram_miss)

    if do_csv:
        write_csv(base_hit, base_miss, oram_hit, oram_miss)

    if not no_plot:
        plot(base_hit, base_miss, oram_hit, oram_miss)

    kill_nfd()

if __name__ == "__main__":
    main()
