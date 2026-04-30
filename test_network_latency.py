#!/usr/bin/env python3
"""
NFD ORAM Network-Latency Comparison
====================================
Tests ORAM overhead at different simulated network RTTs.

Approach:
  A Unix-socket proxy sits between NDN clients (ndnpeek / ndnputchunks)
  and NFD. The proxy adds a configurable one-way delay to every data chunk
  it forwards, simulating a network hop. Both directions are delayed, so
  the injected RTT = 2 × delay_ms.

  Tested delay levels: 0, 5, 10, 25, 50 ms (one-way)  → RTT 0..100 ms

For each delay level the same latency benchmark runs against:
  1. Baseline NFD  (no ORAM)
  2. ORAM NFD      (PathORAM CS + PIT)

The difference  Δ = ORAM_avg − baseline_avg  should stay roughly constant
(it is processing time at the router, not a network effect). As RTT grows
the relative overhead  Δ/RTT  shrinks — ORAM is proportionally cheaper on
slower networks.

Usage:
  python3 test_network_latency.py [--lat-n N] [--oram-only] [--baseline-only]

Options:
  --lat-n N        Samples per latency point   (default: 15)
  --oram-only      Skip baseline NFD
  --baseline-only  Skip ORAM NFD
"""

import os, sys, re, time, signal, socket, threading, subprocess
from dataclasses import dataclass, field
from typing import List, Optional, Tuple, Dict

# ── binary / socket paths ─────────────────────────────────────────────────────

ORAM_BIN     = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "build", "bin", "nfd")
BASELINE_BIN = "/home/w4lker19/Projects/NFD/build/bin/nfd"
NFD_SOCK     = "/tmp/nfd_run/nfd.sock"
NFD_CONF     = "/tmp/nfd_test.conf"
NFD_LOG      = "/tmp/nfd_lat_test.log"
PROXY_SOCK   = "/tmp/nfd_proxy.sock"

# Simulated one-way delays (ms). RTT = 2 × value.
DELAY_LEVELS_MS = [0, 5, 10, 25, 50]

# ── result store ──────────────────────────────────────────────────────────────

@dataclass
class LatPoint:
    delay_ms:  int           # one-way delay injected by proxy
    lats:      List[float] = field(default_factory=list)  # RTT samples (ms)

    @property
    def avg(self): return sum(self.lats) / len(self.lats) if self.lats else 0
    @property
    def p95(self):
        s = sorted(self.lats)
        return s[int(len(s)*0.95)] if s else 0
    @property
    def injected_rtt(self): return self.delay_ms * 2


# ── console helpers ───────────────────────────────────────────────────────────

def banner(t): print(f"\n{'═'*64}\n  {t}\n{'═'*64}")
def section(t): print(f"\n  ── {t}")
def info(m):  print(f"    {m}")


# ── shell helper ──────────────────────────────────────────────────────────────

def run(cmd, timeout=10) -> Tuple[str, int]:
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True,
                           text=True, timeout=timeout)
        return r.stdout.strip(), r.returncode
    except subprocess.TimeoutExpired:
        return "", -1

def tool_exists(name):
    _, rc = run(f"which {name} 2>/dev/null")
    return rc == 0


# ── NFD config (socket in /tmp, no root needed) ───────────────────────────────

def setup_conf():
    os.makedirs(os.path.dirname(NFD_SOCK), exist_ok=True)
    system_conf = "/usr/local/etc/ndn/nfd.conf"
    with open(system_conf) as f:
        text = f.read()
    text = text.replace("path /run/nfd/nfd.sock", f"path {NFD_SOCK}")
    with open(NFD_CONF, "w") as f:
        f.write(text)


# ── NFD lifecycle ─────────────────────────────────────────────────────────────

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
    env["NDN_LOG"] = "nfd.ContentStore=NONE:nfd.Pit=NONE"   # silence logs
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
            info(f"{label} started  (PID={proc.pid})")
            return proc
    info(f"ERROR: {label} did not start within 10 s")
    return None

def stop_nfd(proc):
    if proc is None:
        return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception:
        pass
    time.sleep(0.8)


# ── Socket proxy with configurable one-way delay ──────────────────────────────
#
# NDN clients connect to PROXY_SOCK.  The proxy forwards bytes to NFD_SOCK
# and back, sleeping `delay_ms` ms before every forward chunk.
# Both directions sleep → injected RTT ≈ 2 × delay_ms.
# (This models the consumer→router and router→consumer hops symmetrically.)

class Proxy:
    def __init__(self, delay_ms: float):
        self.delay  = delay_ms / 1000.0
        self._stop  = threading.Event()
        self._srv   = None
        self._thread: Optional[threading.Thread] = None

    def _fwd(self, src: socket.socket, dst: socket.socket):
        try:
            while not self._stop.is_set():
                src.settimeout(0.5)
                try:
                    data = src.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                if self.delay > 0:
                    time.sleep(self.delay)
                dst.sendall(data)
        except Exception:
            pass
        finally:
            try: src.close()
            except: pass
            try: dst.close()
            except: pass

    def _accept_loop(self):
        while not self._stop.is_set():
            self._srv.settimeout(0.5)
            try:
                client, _ = self._srv.accept()
            except socket.timeout:
                continue
            except Exception:
                break
            backend = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                backend.connect(NFD_SOCK)
            except Exception:
                client.close()
                continue
            threading.Thread(target=self._fwd, args=(client, backend),
                             daemon=True).start()
            threading.Thread(target=self._fwd, args=(backend, client),
                             daemon=True).start()

    def start(self):
        if os.path.exists(PROXY_SOCK):
            os.remove(PROXY_SOCK)
        self._srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._srv.bind(PROXY_SOCK)
        self._srv.listen(32)
        self._stop.clear()
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._srv:
            try: self._srv.close()
            except: pass
        if os.path.exists(PROXY_SOCK):
            try: os.remove(PROXY_SOCK)
            except: pass

    def __enter__(self): self.start(); return self
    def __exit__(self, *a): self.stop()


# ── latency measurement at one delay level ────────────────────────────────────

def measure_latency(delay_ms: int, n: int) -> LatPoint:
    pt = LatPoint(delay_ms=delay_ms)

    with Proxy(delay_ms):
        # All NDN tools connect through the proxy
        proxy_env = os.environ.copy()
        proxy_env["NDN_CLIENT_TRANSPORT"] = f"unix://{PROXY_SOCK}"
        time.sleep(0.3)

        # Start a persistent producer through the proxy
        prod = subprocess.Popen(
            'echo "latdata" | ndnputchunks /t/lat/net',
            shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=proxy_env,
        )
        time.sleep(0.5 + delay_ms / 500)  # let it register its route

        for _ in range(n):
            t0 = time.time()
            subprocess.run(
                "ndnpeek --prefix /t/lat/net > /dev/null 2>&1",
                shell=True, env=proxy_env, timeout=10,
            )
            pt.lats.append((time.time() - t0) * 1000)

        prod.terminate(); prod.wait()

    return pt


# ── run suite against one NFD binary ─────────────────────────────────────────

def run_suite(binary: str, label: str, lat_n: int) -> List[LatPoint]:
    banner(f"Testing: {label}")
    kill_nfd()
    time.sleep(0.3)

    proc = start_nfd(binary, label)
    if proc is None:
        return []

    points: List[LatPoint] = []
    for d in DELAY_LEVELS_MS:
        section(f"Simulated one-way delay = {d} ms  (RTT ≈ {d*2} ms)")
        pt = measure_latency(d, lat_n)
        points.append(pt)
        info(f"avg={pt.avg:.1f} ms   p95={pt.p95:.1f} ms   N={lat_n}")

    stop_nfd(proc)
    return points


# ── comparison report ─────────────────────────────────────────────────────────

def print_report(baseline: List[LatPoint], oram: List[LatPoint]):
    banner("RESULTS: ORAM Overhead vs Simulated Network RTT")

    # Header
    print(f"\n  {'Delay 1-way':>12}  {'RTT injected':>13}  "
          f"{'Baseline avg':>13}  {'ORAM avg':>10}  "
          f"{'Δ (overhead)':>13}  {'Overhead %':>11}")
    print(f"  {'─'*12}  {'─'*13}  {'─'*13}  {'─'*10}  {'─'*13}  {'─'*11}")

    for i, d in enumerate(DELAY_LEVELS_MS):
        b_avg = baseline[i].avg if i < len(baseline) else float('nan')
        o_avg = oram[i].avg    if i < len(oram)     else float('nan')
        delta = o_avg - b_avg
        pct   = (delta / b_avg * 100) if b_avg > 0 else float('nan')
        inj   = d * 2
        print(f"  {d:>10} ms  {inj:>11} ms  "
              f"{b_avg:>11.1f} ms  {o_avg:>8.1f} ms  "
              f"{delta:>+11.1f} ms  {pct:>9.1f} %")

    print()
    print("  Key insight:")
    if baseline and oram and len(baseline) == len(oram):
        deltas = [oram[i].avg - baseline[i].avg for i in range(len(DELAY_LEVELS_MS))]
        min_d, max_d = min(deltas), max(deltas)
        pcts = [(oram[i].avg - baseline[i].avg) / baseline[i].avg * 100
                for i in range(len(DELAY_LEVELS_MS)) if baseline[i].avg > 0]
        print(f"  • Absolute overhead Δ stays in [{min_d:.1f} ms, {max_d:.1f} ms]"
              f" (ORAM is router processing, not network)")
        print(f"  • Relative overhead % ranges from {min(pcts):.1f}% to {max(pcts):.1f}%")
        print(f"    → As RTT grows, ORAM becomes proportionally cheaper")
    print()


# ── main ──────────────────────────────────────────────────────────────────────

def check_prerequisites():
    missing = [t for t in ["nfdc", "ndnpeek", "ndnputchunks"] if not tool_exists(t)]
    if missing:
        print("ERROR: missing tools: " + ", ".join(missing))
        sys.exit(1)

def parse_args():
    args = set(sys.argv[1:])
    lat_n = 15
    for a in sys.argv[1:]:
        if a.startswith("--lat-n="):
            try: lat_n = int(a.split("=",1)[1])
            except ValueError: pass
    return {
        "oram_only":     "--oram-only"     in args,
        "baseline_only": "--baseline-only" in args,
        "lat_n":         lat_n,
    }

def main():
    opts = parse_args()

    banner("NFD ORAM Network-Latency Comparison")
    print(f"  Baseline : {BASELINE_BIN}")
    print(f"  ORAM     : {ORAM_BIN}")
    print(f"  Proxy    : {PROXY_SOCK}  (simulates network hops via delay)")
    print(f"  Delays   : {DELAY_LEVELS_MS} ms one-way  "
          f"(RTTs: {[d*2 for d in DELAY_LEVELS_MS]} ms)")
    print(f"  Samples  : {opts['lat_n']} per data point")
    print(f"\n  Theory:")
    print(f"  ORAM adds O(log N) memory operations at the NFD router.")
    print(f"  This is a FIXED processing overhead, independent of network RTT.")
    print(f"  As RTT increases, ORAM overhead stays constant → relatively cheaper.")

    check_prerequisites()
    setup_conf()
    os.environ["NDN_CLIENT_TRANSPORT"] = f"unix://{NFD_SOCK}"

    baseline_pts: List[LatPoint] = []
    oram_pts:     List[LatPoint] = []

    if not opts["oram_only"]:
        baseline_pts = run_suite(BASELINE_BIN, "Baseline NFD (no ORAM)", opts["lat_n"])

    if not opts["baseline_only"]:
        oram_pts = run_suite(ORAM_BIN, "ORAM NFD (PathORAM CS + PIT)", opts["lat_n"])

    if baseline_pts and oram_pts:
        print_report(baseline_pts, oram_pts)
    elif oram_pts:
        banner("ORAM-only results")
        for pt in oram_pts:
            print(f"  delay={pt.delay_ms} ms → avg={pt.avg:.1f} ms  p95={pt.p95:.1f} ms")
    elif baseline_pts:
        banner("Baseline-only results")
        for pt in baseline_pts:
            print(f"  delay={pt.delay_ms} ms → avg={pt.avg:.1f} ms  p95={pt.p95:.1f} ms")

    kill_nfd()

if __name__ == "__main__":
    main()
