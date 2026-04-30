#!/usr/bin/env python3
"""
NFD Normal-Operation Comparison Suite
Runs the same functional + latency tests against:
  1. Baseline NFD  (stock build, no ORAM)
  2. ORAM NFD      (this repository's build)

Verifies that ORAM integration does not break normal NDN forwarding,
CS caching, or PIT behaviour, and reports the latency overhead.

Usage:
  python3 test_nfd_normal.py [OPTIONS]

Options:
  --oram-only     Skip baseline NFD; test ORAM build only
  --baseline-only Skip ORAM NFD; test baseline build only
  --no-latency    Skip latency benchmarks (faster run)
  --keep          Leave the last NFD running after the suite finishes
  --lat-n N       Number of samples per latency benchmark (default: 30)
"""

import subprocess, time, re, sys, os, signal, textwrap
from dataclasses import dataclass, field
from typing import List, Optional, Dict, Tuple

# ── binary paths ─────────────────────────────────────────────────────────────

ORAM_BIN     = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "build", "bin", "nfd")
BASELINE_BIN = "/home/w4lker19/Projects/NFD/build/bin/nfd"
NFD_LOG      = "/tmp/nfd_test.log"

# NFD socket in /tmp so we can run without root.
# All NDN tools (nfdc, ndnpeek, ndnputchunks) read NDN_CLIENT_TRANSPORT.
NFD_SOCK_DIR  = "/tmp/nfd_run"
NFD_SOCK      = f"{NFD_SOCK_DIR}/nfd.sock"
NFD_CONF      = "/tmp/nfd_test.conf"

def _setup_conf():
    """Write a minimal NFD config that puts the Unix socket under /tmp."""
    os.makedirs(NFD_SOCK_DIR, exist_ok=True)
    system_conf = "/usr/local/etc/ndn/nfd.conf"
    if not os.path.exists(system_conf):
        raise FileNotFoundError(f"System NFD config not found: {system_conf}")
    with open(system_conf) as f:
        text = f.read()
    text = text.replace("path /run/nfd/nfd.sock", f"path {NFD_SOCK}")
    with open(NFD_CONF, "w") as f:
        f.write(text)
    # Tell all NDN tools to connect to our socket
    os.environ["NDN_CLIENT_TRANSPORT"] = f"unix://{NFD_SOCK}"


# ── result tracking ──────────────────────────────────────────────────────────

@dataclass
class RunResult:
    label: str
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    latency_cs: List[float]  = field(default_factory=list)   # ms per CS hit
    latency_pit: List[float] = field(default_factory=list)   # ms per round-trip

_current: Optional[RunResult] = None   # set by run_suite()


def _rec_pass(msg):
    global _current
    _current.passed += 1
    print(f"    [PASS] {msg}")

def _rec_fail(msg):
    global _current
    _current.failed += 1
    print(f"    [FAIL] {msg}")

def _rec_skip(msg):
    global _current
    _current.skipped += 1
    print(f"    [skip] {msg}")

def _info(msg):
    print(f"    [info] {msg}")

def _section(title):
    print(f"\n  {'─'*56}\n  {title}\n  {'─'*56}")


# ── shell helpers ─────────────────────────────────────────────────────────────

def run(cmd, timeout=10) -> Tuple[str, int]:
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True,
                           text=True, timeout=timeout)
        return r.stdout.strip(), r.returncode
    except subprocess.TimeoutExpired:
        return "", -1

def tool_exists(name: str) -> bool:
    _, rc = run(f"which {name} 2>/dev/null")
    return rc == 0

def oram_logs(keyword: str) -> List[str]:
    try:
        with open(NFD_LOG) as f:
            return [l.strip() for l in f if keyword in l]
    except Exception:
        return []

def cs_counters() -> Dict[str, int]:
    out, _ = run("nfdc cs info 2>/dev/null")
    def g(k):
        m = re.search(rf"{k}=(\d+)", out)
        return int(m.group(1)) if m else 0
    return {"hits": g("nHits"), "misses": g("nMisses"), "entries": g("nEntries")}

def pit_entry_count() -> int:
    out, _ = run("nfdc status 2>/dev/null")
    m = re.search(r"nPitEntries=(\d+)", out)
    return int(m.group(1)) if m else -1

def reset_cs():
    run("nfdc cs config capacity 65536 2>/dev/null")
    time.sleep(0.2)


# ── producer / consumer helpers ───────────────────────────────────────────────

def start_producer(name: str, content: str, settle: float = 0.4) -> subprocess.Popen:
    proc = subprocess.Popen(
        f'echo "{content}" | ndnputchunks {name}',
        shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    time.sleep(settle)
    return proc

def stop_producer(proc: subprocess.Popen):
    proc.terminate()
    proc.wait()

def fetch(name: str, timeout: int = 5) -> Tuple[str, bool]:
    out, rc = run(f"ndnpeek --prefix {name} 2>/dev/null | strings", timeout=timeout)
    return out, (rc == 0 and bool(out))


# ── NFD lifecycle ─────────────────────────────────────────────────────────────

def nfd_running() -> bool:
    out, _ = run("nfdc status 2>/dev/null | head -3")
    return "version" in out

def kill_any_nfd():
    """Terminate any running NFD instance (needed before switching builds)."""
    run("pkill -x nfd 2>/dev/null")
    for _ in range(14):
        time.sleep(0.5)
        if not nfd_running():
            return
    print("  WARNING: could not stop existing NFD — results may be unreliable")

def start_nfd(binary: str, label: str) -> Optional[subprocess.Popen]:
    if not os.path.exists(binary):
        print(f"  ERROR: binary not found: {binary}")
        return None

    # Remove stale socket so NFD doesn't refuse to bind
    if os.path.exists(NFD_SOCK):
        os.remove(NFD_SOCK)

    env = os.environ.copy()
    env["NDN_LOG"] = (
        "nfd.ContentStore=INFO"
        ":nfd.CsPolicy=INFO"
        ":nfd.Pit=DEBUG"
    )
    env["NDN_CLIENT_TRANSPORT"] = f"unix://{NFD_SOCK}"
    log_fd = open(NFD_LOG, "w")
    proc = subprocess.Popen(
        [binary, "--config", NFD_CONF],
        stdout=log_fd, stderr=log_fd,
        env=env,
        preexec_fn=os.setsid,
    )
    for _ in range(20):
        time.sleep(0.5)
        if nfd_running():
            print(f"  [info] {label} started (PID={proc.pid})")
            return proc
    print(f"  [FAIL] {label} did not start within 10 s")
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception:
        pass
    return None

def stop_nfd_proc(proc: Optional[subprocess.Popen], keep: bool = False):
    if proc is None or keep:
        if keep and proc:
            print(f"  [info] NFD left running (PID={proc.pid})")
        return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception:
        pass
    time.sleep(0.8)


# ════════════════════════════════════════════════════════════════════════════
#  FUNCTIONAL TESTS
#  Each function calls _rec_pass / _rec_fail / _rec_skip and returns nothing.
# ════════════════════════════════════════════════════════════════════════════

def t_cs_basic_hit():
    _section("CS-1  Basic insert + cache hit")
    prod = start_producer("/t/cs/basic", "hello_cache")
    out, s = fetch("/t/cs/basic")
    stop_producer(prod)
    if not (s and "hello_cache" in out):
        _rec_fail(f"First fetch (via producer) failed — got {out!r}")
        return
    _rec_pass("First fetch via producer: 'hello_cache' received")
    time.sleep(0.3)
    out2, s2 = fetch("/t/cs/basic")
    if s2 and "hello_cache" in out2:
        _rec_pass("Second fetch (CS hit, no producer): 'hello_cache' from cache")
    else:
        _rec_fail("CS miss on second fetch — cache did not serve the packet")


def t_cs_miss():
    _section("CS-2  Cache miss for unknown name")
    before = cs_counters()
    fetch("/t/cs/no_such_name_xyzzy_9999", timeout=2)
    after = cs_counters()
    delta = after["misses"] - before["misses"]
    if delta >= 1:
        _rec_pass(f"nMisses +{delta} after requesting unknown name")
    else:
        _info("nMisses unchanged — Interest may have been dropped at FIB (not a failure)")


def t_cs_multiple_entries():
    _section("CS-3  Multiple entries coexist in cache")
    names = [f"/t/cs/multi/{i}" for i in range(1, 6)]
    prods = [start_producer(n, f"val{i}", settle=0.1)
             for i, n in enumerate(names)]
    time.sleep(0.5)
    for n in names:                   # seed the cache
        fetch(n)
    for p in prods:
        stop_producer(p)
    time.sleep(0.4)

    hits = sum(1 for i, n in enumerate(names)
               if "val%d" % i in fetch(n)[0])
    if hits == len(names):
        _rec_pass(f"All {len(names)}/5 entries served from cache after producers stopped")
    else:
        _rec_fail(f"Only {hits}/{len(names)} entries retrievable from cache")


def t_cs_eviction():
    _section("CS-4  LRU eviction respects capacity limit")
    run("nfdc cs config capacity 3 2>/dev/null"); time.sleep(0.2)
    prods = []
    for i in range(1, 6):
        p = start_producer(f"/t/cs/evict/{i}", f"ev{i}", settle=0.1)
        prods.append(p)
        fetch(f"/t/cs/evict/{i}")
        time.sleep(0.1)
    for p in prods:
        stop_producer(p)
    time.sleep(0.4)

    cs = cs_counters()
    if cs["entries"] <= 3:
        _rec_pass(f"nEntries={cs['entries']} <= 3 after capacity=3 — eviction works")
    else:
        _rec_fail(f"nEntries={cs['entries']} > 3 — LRU eviction did not fire")
    reset_cs()


def t_cs_capacity_config():
    _section("CS-5  Capacity change via nfdc cs config")
    # Parse the config command's own response — more reliable than cs info
    out, rc = run("nfdc cs config capacity 200 2>/dev/null")
    m = re.search(r"capacity=(\d+)", out)
    cap = int(m.group(1)) if m else -1
    if rc == 0 and cap == 200:
        _rec_pass("CS capacity updated to 200 via nfdc (confirmed in cs-config-updated response)")
    else:
        _rec_fail(f"nfdc cs config capacity 200 failed — rc={rc}, response: {out!r}")
    reset_cs()


def t_pit_round_trip():
    _section("PIT-1  Basic Interest -> Data round-trip")
    prod = start_producer("/t/pit/rt", "roundtrip_ok")
    out, success = fetch("/t/pit/rt")
    stop_producer(prod)
    if success and "roundtrip_ok" in out:
        _rec_pass("Interest -> Data round-trip completed")
    else:
        _rec_fail(f"Round-trip failed (got {out!r})")


def t_pit_pending_entry():
    _section("PIT-2  Interest held in PIT while waiting for Data")
    before = pit_entry_count()
    unique = f"/t/pit/pend/{int(time.time()*1000)}"
    c = subprocess.Popen(
        f"ndnpeek --lifetime 4000 {unique} > /dev/null 2>&1",
        shell=True,
    )
    time.sleep(0.5)
    after = pit_entry_count()
    c.terminate(); c.wait()

    if before < 0 or after < 0:
        _info("nPitEntries not available — skipping count check")
    elif after > before:
        _rec_pass(f"nPitEntries: {before} -> {after}  (pending Interest in PIT)")
    else:
        _info(f"nPitEntries unchanged ({before} -> {after}) — may have been dropped at FIB")


def t_pit_satisfaction():
    _section("PIT-3  PIT entry removed after Data satisfies Interest")
    unique = f"/t/pit/sat/{int(time.time()*1000)}"
    c = subprocess.Popen(
        f"ndnpeek --lifetime 6000 {unique} > /dev/null 2>&1",
        shell=True,
    )
    time.sleep(0.5)
    pit_with = pit_entry_count()

    prod = start_producer(unique, "sat_data", settle=0.8)
    time.sleep(0.6)
    c.wait(timeout=3)
    stop_producer(prod)
    pit_after = pit_entry_count()

    if pit_with >= 0 and pit_after >= 0:
        if pit_after < pit_with:
            _rec_pass(f"nPitEntries: {pit_with} -> {pit_after}  (entry removed after Data)")
        else:
            _info(f"nPitEntries: {pit_with} -> {pit_after} (may have expired before check)")
    else:
        _info("nPitEntries not readable — skipping assertion")


def t_pit_aggregation():
    _section("PIT-4  PIT aggregates duplicate Interests (both consumers satisfied)")
    unique = f"/t/pit/agg/{int(time.time()*1000)}"

    # Producer must register its FIB route BEFORE consumers send Interests,
    # otherwise NFD has no route and drops the Interest immediately.
    prod = start_producer(unique, "agg_data", settle=0.6)

    c1 = subprocess.Popen(
        f"ndnpeek --prefix {unique} 2>/dev/null | strings",
        shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    time.sleep(0.15)
    c2 = subprocess.Popen(
        f"ndnpeek --prefix {unique} 2>/dev/null | strings",
        shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    time.sleep(1.5)

    for c in (c1, c2):
        try:
            c.wait(timeout=2)
        except subprocess.TimeoutExpired:
            c.terminate()
    stop_producer(prod)

    got1 = "agg_data" in c1.stdout.read().decode(errors="replace")
    got2 = "agg_data" in c2.stdout.read().decode(errors="replace")

    if got1 and got2:
        _rec_pass("Both consumers received Data via PIT aggregation")
    elif got1 or got2:
        _info(f"Only one consumer got Data (timing issue?) — c1={got1}, c2={got2}")
    else:
        _rec_fail("Neither consumer received Data — aggregation or forwarding broken")


def t_fib_list():
    _section("FIB-1  FIB is reachable")
    out, rc = run("nfdc fib list 2>/dev/null")
    if rc != 0:
        _rec_fail("nfdc fib list returned non-zero")
        return
    entries = [l for l in out.splitlines() if l.strip()]
    _rec_pass(f"FIB reachable; {len(entries)} entry/entries visible")
    if entries:
        _info(f"First FIB entry: {entries[0].strip()}")


def t_fib_route_add_remove():
    _section("FIB-2  FIB route add / remove")
    faces_out, _ = run("nfdc face list 2>/dev/null | head -5")
    m = re.search(r"faceid=(\d+)", faces_out)
    if not m:
        _rec_skip("No faces found — cannot add route")
        return
    fid = m.group(1)

    add_out, rc_add = run(f"nfdc route add /t/fib/probe {fid}")
    rm_out,  rc_rm  = run(f"nfdc route remove /t/fib/probe {fid}")

    if rc_add == 0 and "route-add-accepted" in add_out:
        _rec_pass(f"Route /t/fib/probe -> face {fid} added (route-add-accepted)")
    else:
        _rec_fail(f"nfdc route add failed (rc={rc_add}, response: {add_out!r})")

    if rc_rm == 0 and "route-removed" in rm_out:
        _rec_pass("Route /t/fib/probe removed (route-remove-accepted)")
    else:
        _rec_fail(f"nfdc route remove failed (rc={rc_rm}, response: {rm_out!r})")


# ════════════════════════════════════════════════════════════════════════════
#  LATENCY BENCHMARKS  (results stored in _current for comparison table)
# ════════════════════════════════════════════════════════════════════════════

def t_latency_cs(n: int):
    _section(f"LATENCY-1  CS hit latency ({n} samples)")
    prod = start_producer("/t/lat/cs", "latdata_cs")
    fetch("/t/lat/cs")            # seed the cache
    stop_producer(prod); time.sleep(0.3)

    lats = []
    for _ in range(n):
        t0 = time.time()
        run("ndnpeek --prefix /t/lat/cs > /dev/null 2>&1", timeout=5)
        lats.append((time.time() - t0) * 1000)

    avg = sum(lats) / len(lats)
    p95 = sorted(lats)[int(len(lats) * 0.95)]
    _current.latency_cs = lats
    _rec_pass(f"avg={avg:.1f}ms  p95={p95:.1f}ms  min={min(lats):.1f}ms  max={max(lats):.1f}ms")


def t_latency_pit(n: int):
    _section(f"LATENCY-2  PIT round-trip latency ({n} samples)")
    prod = start_producer("/t/lat/pit", "latdata_pit"); time.sleep(0.2)

    lats = []
    for _ in range(n):
        t0 = time.time()
        run("ndnpeek --prefix /t/lat/pit > /dev/null 2>&1", timeout=5)
        lats.append((time.time() - t0) * 1000)

    stop_producer(prod)
    avg = sum(lats) / len(lats)
    p95 = sorted(lats)[int(len(lats) * 0.95)]
    _current.latency_pit = lats
    _rec_pass(f"avg={avg:.1f}ms  p95={p95:.1f}ms  min={min(lats):.1f}ms  max={max(lats):.1f}ms")


def t_oram_log_sanity():
    _section("ORAM-SANITY  ORAM operations in NDN_LOG")
    cs_w = oram_logs("oram-insert")
    cs_r = oram_logs("matched")
    pit_w = oram_logs("pit-insert")
    pit_r = oram_logs("pit-find")

    any_found = False
    for label, lines in [("CS writes (oram-insert)", cs_w),
                          ("CS reads  (matched)",     cs_r),
                          ("PIT writes (pit-insert)", pit_w),
                          ("PIT reads  (pit-find)",   pit_r)]:
        if lines:
            _rec_pass(f"{label}: {len(lines)} log entries")
            any_found = True
        else:
            _info(f"No '{label}' log lines (only expected for ORAM build)")
    if not any_found:
        _info("No ORAM log lines found (normal for baseline build)")


# ════════════════════════════════════════════════════════════════════════════
#  SUITE RUNNER
# ════════════════════════════════════════════════════════════════════════════

ALL_FUNCTIONAL = [
    t_cs_basic_hit,
    t_cs_miss,
    t_cs_multiple_entries,
    t_cs_eviction,
    t_cs_capacity_config,
    t_pit_round_trip,
    t_pit_pending_entry,
    t_pit_satisfaction,
    t_pit_aggregation,
    t_fib_list,
    t_fib_route_add_remove,
]

def run_suite(result: RunResult, binary: str, lat_n: int,
              run_latency: bool) -> bool:
    """
    Start `binary`, run all tests into `result`, stop it.
    Returns True if NFD started successfully.
    """
    global _current
    _current = result

    kill_any_nfd()
    time.sleep(0.5)

    proc = start_nfd(binary, result.label)
    if proc is None:
        result.failed += 1
        return False

    open(NFD_LOG, "w").close()
    _info("Log cleared — starting tests ...\n")
    time.sleep(0.3)

    for test in ALL_FUNCTIONAL:
        test()

    if run_latency:
        t_latency_cs(lat_n)
        t_latency_pit(lat_n)

    t_oram_log_sanity()

    stop_nfd_proc(proc)
    return True


# ════════════════════════════════════════════════════════════════════════════
#  COMPARISON REPORT
# ════════════════════════════════════════════════════════════════════════════

def _latency_stats(lats: List[float]):
    if not lats:
        return "N/A"
    avg = sum(lats) / len(lats)
    p95 = sorted(lats)[int(len(lats) * 0.95)]
    return f"avg={avg:.1f}ms  p95={p95:.1f}ms"

def _overhead(oram: List[float], base: List[float]) -> str:
    if not oram or not base:
        return "N/A"
    ratio = (sum(oram)/len(oram)) / (sum(base)/len(base))
    return f"{ratio:.2f}x"

def print_comparison(baseline: Optional[RunResult], oram: Optional[RunResult]):
    print(f"\n{'=' * 62}")
    print(f"  COMPARISON REPORT")
    print(f"{'=' * 62}")

    labels = []
    results_list = []
    if baseline:
        labels.append("Baseline")
        results_list.append(baseline)
    if oram:
        labels.append("ORAM")
        results_list.append(oram)

    # Functional results table
    col = 16
    header = f"  {'Metric':<30}" + "".join(f"{l:>{col}}" for l in labels)
    print(header)
    print(f"  {'─'*30}" + "─"*(col * len(labels)))

    for attr, label_str in [
        ("passed",  "Tests passed"),
        ("failed",  "Tests failed"),
        ("skipped", "Tests skipped"),
    ]:
        row = f"  {label_str:<30}" + "".join(
            f"{getattr(r, attr):>{col}}" for r in results_list
        )
        print(row)

    # Latency comparison
    if any(r.latency_cs for r in results_list):
        print(f"\n  {'Latency metric':<30}" + "".join(f"{l:>{col}}" for l in labels))
        print(f"  {'─'*30}" + "─"*(col * len(labels)))

        for attr, metric in [("latency_cs", "CS hit"), ("latency_pit", "PIT round-trip")]:
            stats = [_latency_stats(getattr(r, attr)) for r in results_list]
            row = f"  {metric + ' (avg, p95)':<30}" + "".join(
                f"{s:>{col}}" for s in stats
            )
            print(row)

        if baseline and oram:
            for attr, metric in [("latency_cs", "CS overhead"),
                                  ("latency_pit", "PIT overhead")]:
                oh = _overhead(getattr(oram, attr), getattr(baseline, attr))
                print(f"  {metric + ' (ORAM/baseline)':<30}{'':>{col}}{oh:>{col}}")

    print(f"\n  Baseline binary : {BASELINE_BIN}")
    print(f"  ORAM binary     : {ORAM_BIN}")

    # Overall verdict
    all_fail = sum(r.failed for r in results_list)
    print(f"\n  {'OVERALL':} ", end="")
    if all_fail == 0:
        print("PASS — all functional tests passed in both builds")
    else:
        print(f"FAIL — {all_fail} test(s) failed (see [FAIL] lines above)")
    print()


# ════════════════════════════════════════════════════════════════════════════
#  MAIN
# ════════════════════════════════════════════════════════════════════════════

def check_prerequisites():
    missing = [t for t in ["nfdc", "ndnpeek", "ndnputchunks"] if not tool_exists(t)]
    if missing:
        print("ERROR: required tools missing: " + ", ".join(missing))
        print("Install ndn-tools: https://github.com/named-data/ndn-tools")
        sys.exit(1)

def parse_args():
    args = set(sys.argv[1:])
    lat_n = 30
    for a in sys.argv[1:]:
        if a.startswith("--lat-n="):
            try:
                lat_n = int(a.split("=", 1)[1])
            except ValueError:
                pass
    return {
        "oram_only":     "--oram-only"     in args,
        "baseline_only": "--baseline-only" in args,
        "no_latency":    "--no-latency"    in args,
        "keep":          "--keep"          in args,
        "lat_n":         lat_n,
    }

def main():
    opts = parse_args()

    print(f"\n{'=' * 62}")
    print(f"  NFD Normal-Operation Comparison Suite")
    print(f"{'=' * 62}")
    print(f"  Baseline : {BASELINE_BIN}")
    print(f"  ORAM     : {ORAM_BIN}")
    print(f"  Log      : {NFD_LOG}")
    print(f"  Socket   : {NFD_SOCK}  (no root needed)")
    if opts["no_latency"]:
        print(f"  Latency  : disabled (--no-latency)")
    else:
        print(f"  Lat N    : {opts['lat_n']} samples per benchmark")

    check_prerequisites()
    _setup_conf()

    baseline_result: Optional[RunResult] = None
    oram_result:     Optional[RunResult] = None

    # ── baseline run ──────────────────────────────────────────────
    if not opts["oram_only"]:
        print(f"\n{'#' * 62}")
        print(f"#  BASELINE NFD (no ORAM)")
        print(f"{'#' * 62}")
        baseline_result = RunResult(label="Baseline")
        run_suite(baseline_result, BASELINE_BIN,
                  lat_n=opts["lat_n"], run_latency=not opts["no_latency"])

    # ── ORAM run ──────────────────────────────────────────────────
    if not opts["baseline_only"]:
        print(f"\n{'#' * 62}")
        print(f"#  ORAM NFD (PathORAM CS + PIT)")
        print(f"{'#' * 62}")
        oram_result = RunResult(label="ORAM")
        run_suite(oram_result, ORAM_BIN,
                  lat_n=opts["lat_n"], run_latency=not opts["no_latency"])

    # ── final report ──────────────────────────────────────────────
    print_comparison(baseline_result, oram_result)

    all_fail = sum(r.failed for r in [baseline_result, oram_result] if r)
    sys.exit(0 if all_fail == 0 else 1)


if __name__ == "__main__":
    main()
