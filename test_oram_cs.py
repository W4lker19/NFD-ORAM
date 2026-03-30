#!/usr/bin/env python3
"""
NFD ORAM Content Store - Test Suite
Testa automaticamente: insert, hit, miss, eviction e latência
"""

import subprocess, time, re, sys, os, signal

NFD_BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "bin", "nfd")
NFD_LOG = "/tmp/nfd.log"
SEP = "=" * 60

def run(cmd, timeout=10):
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip(), r.returncode
    except subprocess.TimeoutExpired:
        return "", -1

def section(t): print(f"\n{SEP}\n {t}\n{SEP}")
def ok(m):   print(f"  ✅ {m}")
def fail(m): print(f"  ❌ {m}")
def info(m): print(f"  ℹ️  {m}")

def start_nfd():
    out, _ = run("nfdc status 2>/dev/null | head -2")
    if "version" in out:
        with open(NFD_LOG, "a"):
            pass
        run("nfdc cs info > /dev/null 2>&1"); time.sleep(0.3)
        with open(NFD_LOG) as f:
            content = f.read()
        if "nfd.ContentStore" not in content:
            fail("NFD a correr MAS sem logs de ContentStore!")
            info("Para com: sudo pkill -x nfd")
            info("Reinicia com:")
            info('  sudo NDN_LOG="nfd.ContentStore=INFO:nfd.CsPolicy=INFO" \\')
            info('  ./build/bin/nfd > /tmp/nfd.log 2>&1 &')
            sys.exit(1)
        ok("NFD já está a correr — NÃO será parado no fim")
        return None

    info("NFD não está a correr — a iniciar...")
    env = os.environ.copy()
    env["NDN_LOG"] = "nfd.ContentStore=INFO:nfd.CsPolicy=INFO"
    log_fd = open(NFD_LOG, "w")
    proc = subprocess.Popen(
        [NFD_BIN],
        stdout=log_fd, stderr=log_fd,
        env=env,
        preexec_fn=os.setsid,
    )
    for i in range(20):
        time.sleep(0.5)
        out, rc = run("nfdc status 2>/dev/null | head -2")
        if "version" in out:
            ok(f"NFD iniciado (PID={proc.pid})")
            return proc
    fail("NFD não arrancou após 10s")
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception:
        pass
    sys.exit(1)

def stop_nfd(proc):
    if proc is None:
        info("NFD foi iniciado externamente — a deixar a correr")
        return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except Exception:
        pass
    time.sleep(1)
    ok("NFD terminado")

def publish_and_peek(name, content):
    pub = subprocess.Popen(f'echo "{content}" | ndnputchunks {name}',
                           shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.5)
    out, _ = run(f"ndnpeek --prefix {name} 2>/dev/null | strings", timeout=5)
    pub.terminate(); pub.wait(); time.sleep(0.3)
    return content in out

def peek_cs(name, content):
    out, _ = run(f"ndnpeek --prefix {name} 2>/dev/null | strings", timeout=5)
    return content in out

def cs_info():
    out, _ = run("nfdc cs info 2>/dev/null")
    def g(k): m = re.search(rf"{k}=(\d+)", out); return int(m.group(1)) if m else 0
    return {"hits": g("nHits"), "misses": g("nMisses"), "entries": g("nEntries")}

def oram_logs(kw):
    try:
        with open(NFD_LOG) as f:
            return [l.strip() for l in f if kw in l and "localhost" not in l]
    except:
        return []

# ── TESTE 1 — Insert básico + CS hit ──────────────────────────
def test_basic(nfd_proc):
    section("TESTE 1 — Insert básico e CS hit")
    info("Publica /test/basic | 1º peek via producer | 2º peek via CS")

    if publish_and_peek("/test/basic", "hello"):
        ok("1º peek: 'hello' recebido via producer")
    else:
        fail("1º peek falhou")

    reads_before = len(oram_logs("matched /test/basic"))

    hit = peek_cs("/test/basic", "hello")
    time.sleep(0.2)

    reads_after = len(oram_logs("matched /test/basic"))

    if hit:
        ok("2º peek: 'hello' recebido via CS (producer morto)")
    else:
        fail("2º peek: CS miss")

    gained = reads_after - reads_before
    if gained >= 1:
        logs = oram_logs("matched")
        ok(f"ORAM READ confirmado (+{gained}): {logs[-1].split('INFO:')[-1].strip()}")
    else:
        fail("Nenhum ORAM READ no log para /test/basic")

    logs = oram_logs("oram-insert")
    if logs:
        ok(f"ORAM WRITE: {logs[-1].split('INFO:')[-1].strip()}")
    else:
        fail("Sem logs oram-insert")

# ── TESTE 2 — Múltiplos nomes ─────────────────────────────────
def test_multiple(nfd_proc):
    section("TESTE 2 — Múltiplos nomes no ORAM")
    info("5 Data packets diferentes → todos devem ser CS hits após producers morrerem")

    for i in range(1, 6):
        publish_and_peek(f"/test/multi/item{i}", f"content{i}")

    passed = 0
    for i in range(1, 6):
        hit = peek_cs(f"/test/multi/item{i}", f"content{i}")
        if hit:
            ok(f"CS hit: /test/multi/item{i}")
            passed += 1
        else:
            fail(f"CS miss: /test/multi/item{i}")

    block_ids = []
    for l in oram_logs("oram-insert"):
        m = re.search(r"blockId=(\d+)", l)
        if m and any(f"item{i}" in l for i in range(1, 6)):
            block_ids.append(int(m.group(1)))

    if block_ids:
        info(f"blockIds atribuídos: {', '.join(str(x) for x in block_ids[-5:])}")
        if all(0 <= x < 1024 for x in block_ids):
            ok(f"Todos os blockIds em range [0, 1024) — ORAM_CAPACITY=1024 ✅")
        else:
            fail(f"blockId fora de range [0, 1024): {block_ids}")

    info(f"Resultado: {passed}/5 CS hits")

# ── TESTE 3 — Eviction LRU ────────────────────────────────────
def test_eviction(nfd_proc):
    section("TESTE 3 — Eviction LRU com ORAM")
    info("Capacidade=3, insere 5 packets → LRU deve evict os mais antigos")

    run("nfdc cs config capacity 3 2>/dev/null"); time.sleep(0.3)
    for i in range(1, 6):
        publish_and_peek(f"/test/evict/item{i}", f"evdata{i}")
        time.sleep(0.1)

    cs = cs_info()
    if cs["entries"] <= 3:
        ok(f"nEntries={cs['entries']} ≤ 3 — LRU eviction funcionou")
    else:
        fail(f"nEntries={cs['entries']} > 3 — eviction não ocorreu")

    run("nfdc cs config capacity 65536 2>/dev/null")

# ── TESTE 4 — Latência ────────────────────────────────────────
def test_latency(nfd_proc):
    section("TESTE 4 — Latência de CS hits com ORAM")
    info("50 CS hits consecutivos ao mesmo Data packet")

    publish_and_peek("/test/lat", "latency"); time.sleep(0.5)

    lats = []
    for _ in range(50):
        t0 = time.time()
        run("ndnpeek --prefix /test/lat > /dev/null 2>&1", timeout=5)
        lats.append((time.time() - t0) * 1000)

    avg = sum(lats) / len(lats)
    mn  = min(lats)
    mx  = max(lats)
    p95 = sorted(lats)[int(len(lats) * 0.95)]
    ok(f"Média={avg:.1f}ms | Min={mn:.1f}ms | Max={mx:.1f}ms | P95={p95:.1f}ms (N=50)")
    info("Overhead Path ORAM: O(log N) acessos por operação")

# ── MAIN ──────────────────────────────────────────────────────
def main():
    keep = "--keep" in sys.argv

    print(f"\n{SEP}\n NFD ORAM Content Store — Test Suite\n{SEP}")
    info(f"Binary : {NFD_BIN}")
    info(f"Log    : {NFD_LOG}")
    if keep:
        info("Modo --keep: NFD ficará a correr após os testes")

    nfd_proc = start_nfd()

    open(NFD_LOG, "w").close()
    info("Log limpo — a iniciar testes...")
    time.sleep(0.3)

    try:
        test_basic(nfd_proc)
        test_multiple(nfd_proc)
        test_eviction(nfd_proc)
        test_latency(nfd_proc)
    finally:
        section("FIM DOS TESTES")
        cs = cs_info()
        info(f"nHits={cs['hits']} nMisses={cs['misses']} nEntries={cs['entries']}")

        if keep:
            info("NFD continua a correr (--keep)")
            if nfd_proc:
                info(f"Para parar: sudo kill {nfd_proc.pid}")
        else:
            section("A parar o NFD")
            stop_nfd(nfd_proc)

if __name__ == "__main__":
    main()