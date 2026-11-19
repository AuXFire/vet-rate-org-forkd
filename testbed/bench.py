#!/usr/bin/env python3
"""lean testbed — capture real tool output, then measure lean against it.

  python3 testbed/bench.py capture [names…] [--force]   run scenarios for real, cache raw output
  python3 testbed/bench.py eval [names…] [-v]           retention / false-success / tokens / latency
  python3 testbed/bench.py loop                         fix-loop episodes: total context cost raw vs lean
  python3 testbed/bench.py fuzz [n]                     robustness: random & adversarial inputs
  python3 testbed/bench.py perf                         hook + statusline latency
  python3 testbed/bench.py outline                      `o` accuracy vs compiler ground truth
  python3 testbed/bench.py all                          everything above (no recapture) + REPORT.md
"""
import hashlib, json, os, random, re, shutil, statistics, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, ".work")
CAPS = os.path.join(HERE, "captures")
LEAN_ROOT = os.environ.get("LEAN_ROOT") or ("/home/user/vet-rate-org-forkd" if os.path.isdir("/home/user/vet-rate-org-forkd/lib/lean") else os.path.dirname(HERE))
os.environ["LEAN_HOME"] = os.environ.get("LEAN_TEST_HOME") or tempfile.mkdtemp(prefix="lean-bench-")
os.environ.setdefault("LEAN_REPORT_MAX_AGE", str(10 ** 9))   # captured reports are older than a live run's
sys.path.insert(0, os.path.join(LEAN_ROOT, "lib"))
sys.path.insert(0, HERE)
from lean import filt, hook, jsonc, nav, run, usage, util  # noqa: E402
import oracle  # noqa: E402
from projects import PROJECTS, SETUP, SHARED_DIRS  # noqa: E402
from scenarios import SCENARIOS, SETUP_CMDS  # noqa: E402

QUIET = set(util.QUIET_ENV) | {"LEAN_SID"}
T = util.tokens


def base_env():
    """What the agent's shell looks like WITHOUT lean (no quiet vars)."""
    return {k: v for k, v in os.environ.items() if k not in QUIET}


def sh(cmd, cwd, timeout=900, env=None):
    t0 = time.time()
    p = subprocess.run(cmd, shell=True, cwd=cwd, env=env or base_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace"), time.time() - t0


# ---------------------------------------------------------------- materialize
def shared(project):
    d = os.path.join(WORK, "_shared", project)
    if project in SETUP and not os.path.exists(os.path.join(d, ".ok")):
        os.makedirs(d, exist_ok=True)
        for f, c in PROJECTS[project].items():
            _write(os.path.join(d, f), c)
        code, out, _ = sh(SETUP[project], d)
        if code != 0:
            raise SystemExit("setup %s failed:\n%s" % (project, out[-2000:]))
        open(os.path.join(d, ".ok"), "w").close()
    return d


def _write(p, c):
    os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
    with open(p, "w") as f:
        f.write(c)


def materialize(sc):
    d = os.path.join(WORK, sc["name"])
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    for f, c in dict(PROJECTS[sc["project"]], **sc["files"]).items():
        _write(os.path.join(d, f), c)
    for sd in SHARED_DIRS.get(sc["project"], []):
        os.symlink(os.path.join(shared(sc["project"]), sd), os.path.join(d, sd))
    if sc["project"] == "rust":
        os.environ.setdefault("CARGO_TARGET_DIR", os.path.join(WORK, "_cargo_target"))
    if sc["name"] in SETUP_CMDS:
        sh(SETUP_CMDS[sc["name"]], d)
    return d


def cap_path(name):
    return os.path.join(CAPS, name + ".json")


def capture(names, force=False):
    os.makedirs(CAPS, exist_ok=True)
    for sc in SCENARIOS:
        if names and sc["name"] not in names:
            continue
        if os.path.exists(cap_path(sc["name"])) and not force:
            continue
        d = materialize(sc)
        if sc["project"] == "rust":   # isolate per scenario so compile output is always present
            os.environ["CARGO_TARGET_DIR"] = os.path.join(WORK, "_cargo_target", sc["name"])
        code, raw, dt = sh(sc["cmd"], d)
        with open(cap_path(sc["name"]), "w") as f:
            json.dump({"cmd": sc["cmd"], "code": code, "raw": raw, "dt": round(dt, 2)}, f)
        print("captured %-24s exit %-3d %6d chars %5.1fs" % (sc["name"], code, len(raw), dt))


def load(name):
    with open(cap_path(name)) as f:
        return json.load(f)


# ---------------------------------------------------------------- eval
def auto_view(sc, cap, cwd):
    """What lean shows automatically (PostToolUse) — failures pass through unchanged by Claude Code."""
    if sc["note"].startswith("tool:"):
        tool = sc["note"][5:]
        lines = [l for l in cap["raw"].split("\n") if l]
        tr = {"filenames": [os.path.join(cwd, l) for l in lines], "numFiles": len(lines)} if tool == "Glob" else \
             {"mode": "content", "content": "\n".join(os.path.join(cwd, l) if not l.startswith("/") else l for l in lines), "numLines": len(lines)}
        out = []
        old = hook.emit
        hook.emit = out.append
        try:
            hook.on_post_search({"hook_event_name": "PostToolUse", "tool_name": tool, "session_id": "bench", "tool_input": {}, "tool_response": tr})
        finally:
            hook.emit = old
        if not out:
            return "\n".join(tr.get("filenames") or [tr.get("content", "")]), "unchanged"
        new = out[0]["hookSpecificOutput"]["updatedToolOutput"]
        shown = new["filenames"][0] if tool == "Glob" else new["content"]
        # losslessness: expanded view must equal the original
        orig = tr["filenames"] if tool == "Glob" else tr["content"].split("\n")
        if tool == "Glob" and filt.expand_paths(shown) != orig:
            return shown + "\n[LOSSY]", "filtered"
        return shown, "filtered"
    if cap["code"] != 0:
        return cap["raw"], "passthrough"
    ev = {"hook_event_name": "PostToolUse", "tool_name": "Bash", "session_id": "bench", "cwd": cwd,
          "tool_input": {"command": sc["cmd"]}, "tool_response": {"stdout": cap["raw"], "stderr": "", "interrupted": False, "isImage": False}}
    out = []
    old = hook.emit
    hook.emit = out.append
    try:
        hook.on_post_bash(ev)
    finally:
        hook.emit = old
    if out:
        return out[0]["hookSpecificOutput"]["updatedToolOutput"]["stdout"], "filtered"
    return cap["raw"], "unchanged"


def wrapped_view(sc, cap, cwd):
    text, _, kind = run.render(sc["cmd"], cap["raw"], cap["code"], cap["dt"], cwd, show_cmd=False)
    return text, kind


def judge(sc, cap, shown):
    facts = list(sc["facts"]) + oracle.facts(sc, cap["raw"])
    missing = [f for f in facts if not oracle.present(f, shown)]
    first = shown.strip().split("\n", 1)[0] if shown.strip() else ""
    false_ok = sc["expect"] == "fail" and first.startswith("✓")
    false_fail = sc["expect"] == "pass" and first.startswith("✗")
    return facts, missing, false_ok, false_fail


def evaluate(names=(), verbose=False):
    rows, problems = [], []
    for sc in SCENARIOS:
        if names and sc["name"] not in names:
            continue
        if not os.path.exists(cap_path(sc["name"])):
            print("skip %s (not captured)" % sc["name"])
            continue
        cap = load(sc["name"])
        cwd = os.path.join(WORK, sc["name"]) if sc["note"] == "xml-reports" and os.path.isdir(os.path.join(WORK, sc["name"])) else materialize(sc)
        util.save_state(util.root(cwd), {})        # no Δ carry-over between scenarios
        t0 = time.time()
        wtext, kind = wrapped_view(sc, cap, cwd)
        lat = (time.time() - t0) * 1000
        util.save_state(util.root(cwd), {})
        atext, amode = auto_view(sc, cap, cwd)
        if sc["note"].startswith("tool:"):
            wtext, kind = atext, sc["note"]
            r_raw = len(util.clean(cap["raw"])) + cap["raw"].count("\n") * len(cwd)   # tools return absolute paths
        if "[LOSSY]" in atext:
            print("LOSSY compaction in", sc["name"])
        raw = util.clean(cap["raw"])
        if sc["note"].startswith("tool:"):
            raw = "x" * r_raw
        if sc["note"] == "paths":
            exp = filt.expand_paths(wtext) if "(" in wtext.split("\n", 1)[0] else wtext.split("\n")
            if [l for l in exp if l] != [l for l in cap["raw"].split("\n") if l]:
                print("LOSSY path compaction in", sc["name"])
                wtext += "\n[LOSSY]"
        facts, missing, false_ok, false_fail = judge(sc, cap, wtext)
        _, amissing, afalse_ok, _ = judge(sc, cap, atext) if amode == "filtered" else (None, [], False, False)
        r = dict(name=sc["name"], kind=kind, code=cap["code"], raw=T(len(raw)), wrapped=T(len(wtext)), auto=T(len(atext)), amode=amode,
                 facts=len(facts), missing=missing, amissing=amissing, false_ok=false_ok or afalse_ok, false_fail=false_fail, ms=round(lat, 1),
                 grew=len(wtext) > len(raw) + 200)
        rows.append(r)
        bad = []
        if "[LOSSY]" in wtext or "[LOSSY]" in atext:
            bad.append("LOSSY")
        if missing:
            bad.append("lost %s" % missing)
        if amissing:
            bad.append("auto lost %s" % amissing)
        if r["false_ok"]:
            bad.append("FALSE SUCCESS")
        if false_fail:
            bad.append("false failure")
        if r["grew"]:
            bad.append("output grew")
        if bad:
            problems.append((sc["name"], bad))
        if verbose or bad:
            print("\n===== %s [%s] raw %d → wrapped %d tok (auto %s %d)\n%s" % (sc["name"], kind, r["raw"], r["wrapped"], amode, r["auto"], wtext))
    tot_raw = sum(r["raw"] for r in rows)
    tot_w = sum(r["wrapped"] for r in rows)
    print("\n%-24s %-8s %5s %7s %7s %7s %6s %s" % ("scenario", "kind", "exit", "raw", "lean", "saved", "ms", "facts"))
    for r in rows:
        print("%-24s %-8s %5d %7d %7d %6.0f%% %6.1f %d/%d%s" % (r["name"], r["kind"], r["code"], r["raw"], r["wrapped"],
              100 * (1 - r["wrapped"] / max(r["raw"], 1)), r["ms"], r["facts"] - len(r["missing"]), r["facts"], "  ⚠" if r["missing"] or r["false_ok"] else ""))
    print("TOTAL raw %d tok → lean %d tok (%.1f%% saved) · problems: %d" % (tot_raw, tot_w, 100 * (1 - tot_w / max(tot_raw, 1)), len(problems)))
    for n, b in problems:
        print("  ✗ %s: %s" % (n, "; ".join(b)))
    return rows, problems


# ---------------------------------------------------------------- loop (fix episodes, real execution)
LOOPS = {
    "py": ("python3 -m pytest", [
        ("calc/core.py", "a + b + (1 if a > 5 else 0)", "a + b"),
        ("calc/core.py", "user['name'].strip()", "user.get('name', user.get('nm', '')).strip()"),
        ("tests/test_core.py", "[1, 2, 4]", "[1, 2, 3]"),
        ("calc/core.py", "return a / b", "return a / b if b else 0"),
        ("tests/test_fmt.py", "'0.0%'", "'0.0%'"),
    ]),
    "vitest": ("npx vitest run", [
        ("src/sum.ts", "a + b + (a > 5 ? 1 : 0)", "a + b"),
        ("src/sum.ts", "u.profile!.name", "u.profile?.name ?? 'x'"),
        ("src/sum.test.ts", "[1, 2, 4]", "[1, 2, 3]"),
    ]),
    "go": ("go test ./...", [("calc.go", "return a + b + 1", "return a + b")]),
    "rust": ("cargo test", [("src/lib.rs", "a + b + 1", "a + b"), ("src/lib.rs", "v[0]", "*v.first().unwrap_or(&0)")]),
    "jest": ("npx jest", [("sum.js", "a + b + (a > 5 ? 1 : 0)", "a + b"), ("sum.js", "u.profile.name", "(u.profile || {name: 'x'}).name")]),
}
TURNS_BETWEEN_RUNS = 3   # requests the agent makes between test runs (read, edit, …)


def loop():
    lean_bin = os.path.join(LEAN_ROOT, "bin", "lean")
    tbin = os.path.join(LEAN_ROOT, "bin", "t")
    print("%-8s %5s %10s %10s %10s %10s %7s" % ("project", "runs", "raw out", "lean out", "raw ctx", "lean ctx", "saved"))
    totals = [0, 0, 0, 0]
    for proj, (cmd, fixes) in LOOPS.items():
        base = materialize({"name": "loop-" + proj, "project": proj, "files": {}})
        if proj == "rust":
            os.environ["CARGO_TARGET_DIR"] = os.path.join(WORK, "_cargo_target", "loop")
        sh("git init -q . && git add -A && git -c user.email=t@t -c user.name=t commit -qm init", base)
        env = dict(base_env(), LEAN_HOME=os.environ["LEAN_HOME"])
        raw_outs, lean_outs = [], []
        steps = [None] + fixes
        for i, fx in enumerate(steps):
            if fx:
                p = os.path.join(base, fx[0])
                s = open(p).read()
                open(p, "w").write(s.replace(fx[1], fx[2]))
            _, raw, _ = sh(cmd, base)
            raw_outs.append(T(len(raw)))
            _, lo, _ = sh("%s" % tbin, base, env=env)
            lean_outs.append(T(len(lo)))
        n = len(steps)
        # context cost: each output is re-read by every later request in the episode
        def ctx(outs):
            return sum(o * (1 + TURNS_BETWEEN_RUNS * (n - 1 - i)) for i, o in enumerate(outs))
        rc, lc = ctx(raw_outs), ctx(lean_outs)
        for k, v in enumerate((sum(raw_outs), sum(lean_outs), rc, lc)):
            totals[k] += v
        print("%-8s %5d %10d %10d %10d %10d %6.0f%%" % (proj, n, sum(raw_outs), sum(lean_outs), rc, lc, 100 * (1 - lc / max(rc, 1))))
    print("%-8s %5s %10d %10d %10d %10d %6.0f%%" % ("TOTAL", "", totals[0], totals[1], totals[2], totals[3], 100 * (1 - totals[3] / max(totals[2], 1))))
    return totals


# ---------------------------------------------------------------- fuzz
def fuzz(n=400):
    rnd = random.Random(7)
    alphabet = "abc XYZ 012 \t\x1b[31m\x1b[0m\r\n\n\n✓✗→ é 中文 {}[]\":,|\\/" + "".join(chr(i) for i in range(1, 32))
    cmds = ["pytest", "npm test", "cargo build", "npm install", "git push", "git diff", "docker build .", "make", "python3 x.py", "ls", "cat f", "x | y"]
    crashes, grew, slow = [], 0, []
    gens = [
        lambda: "".join(rnd.choice(alphabet) for _ in range(rnd.randint(0, 5000))),
        lambda: "\n".join("same line" for _ in range(rnd.randint(1, 20000))),
        lambda: "x" * rnd.randint(1, 300000),
        lambda: "{" * rnd.randint(1, 5000),
        lambda: json.dumps([{"a": i, "b": [i] * 3} for i in range(rnd.randint(0, 3000))], indent=rnd.choice([None, 2])),
        lambda: "\n".join("FAILED tests/t.py::t%d - AssertionError" % i for i in range(rnd.randint(0, 3000))),
        lambda: "\n".join("src/a.ts(%d,1): error TS2322: bad" % i for i in range(rnd.randint(0, 3000))),
        lambda: bytes(rnd.getrandbits(8) for _ in range(rnd.randint(0, 20000))).decode("utf-8", "replace"),
        lambda: "\r".join("progress %d%%" % i for i in range(rnd.randint(0, 20000))),
        lambda: "=" * 30 + " FAILURES " + "=" * 30 + "\n" + "_" * 10 + " t " + "_" * 10 + "\n" * rnd.randint(0, 5000),
    ]
    for i in range(n):
        txt = gens[i % len(gens)]()
        cmd = rnd.choice(cmds)
        t0 = time.time()
        try:
            out, _ = filt.filter_output(cmd, txt, tempfile.gettempdir(), code=rnd.choice([None, 0, 1]))
        except Exception as e:  # noqa: BLE001
            crashes.append((cmd, i % len(gens), repr(e)[:200]))
            continue
        dt = time.time() - t0
        if dt > 1.0:
            slow.append((cmd, i % len(gens), len(txt), round(dt, 2)))
        if len(out) > len(util.clean(txt)) + 400:
            grew += 1
    print("fuzz: %d cases · crashes %d · grew>400 chars %d · slow(>1s) %d" % (n, len(crashes), grew, len(slow)))
    for c in crashes[:10]:
        print("  crash", c)
    for s in slow[:10]:
        print("  slow", s)
    return crashes, grew, slow


# ---------------------------------------------------------------- perf
def perf():
    lean_bin = os.path.join(LEAN_ROOT, "bin", "lean")
    env = dict(os.environ)
    res = {}
    raw = load("py-big-pass-verbose")["raw"] if os.path.exists(cap_path("py-big-pass-verbose")) else "x\n" * 5000
    events = {
        "bash-big": {"hook_event_name": "PostToolUse", "tool_name": "Bash", "session_id": "perf", "cwd": HERE, "tool_input": {"command": "pytest -v"}, "tool_response": {"stdout": raw, "stderr": ""}},
        "bash-small": {"hook_event_name": "PostToolUse", "tool_name": "Bash", "session_id": "perf", "cwd": HERE, "tool_input": {"command": "ls"}, "tool_response": {"stdout": "a\nb", "stderr": ""}},
        "pre-bash": {"hook_event_name": "PreToolUse", "tool_name": "Bash", "session_id": "perf", "cwd": HERE, "permission_mode": "default", "tool_input": {"command": "pytest"}},
        "session-start": {"hook_event_name": "SessionStart", "session_id": "perf", "cwd": HERE, "source": "startup"},
    }
    for name, ev in events.items():
        ts = []
        data = json.dumps(ev).encode()
        for _ in range(7):
            t0 = time.time()
            subprocess.run([lean_bin, "hook"], input=data, stdout=subprocess.DEVNULL, env=env)
            ts.append((time.time() - t0) * 1000)
        res[name] = (statistics.median(ts), max(ts))
    # statusline over a large synthetic transcript
    d = tempfile.mkdtemp()
    tp = os.path.join(d, "s.jsonl")
    with open(tp, "w") as f:
        for i in range(5000):
            f.write(json.dumps({"type": "assistant", "timestamp": "2026-11-19T10:00:00Z", "message": {"id": "m%d" % i, "model": "claude-opus-5-5",
                    "usage": {"input_tokens": 3, "cache_creation_input_tokens": 500, "cache_read_input_tokens": 100000 + i, "output_tokens": 300}}}) + "\n")
    ev = json.dumps({"session_id": "s", "transcript_path": tp}).encode()
    ts = []
    for k in range(5):
        t0 = time.time()
        subprocess.run([lean_bin, "statusline"], input=ev, stdout=subprocess.DEVNULL, env=env)
        ts.append((time.time() - t0) * 1000)
    res["statusline-5k-cold"] = (ts[0], ts[0])
    res["statusline-5k-warm"] = (statistics.median(ts[1:]), max(ts[1:]))
    for k, (med, mx) in res.items():
        print("perf %-20s median %6.1f ms  max %6.1f ms" % (k, med, mx))
    return res


# ---------------------------------------------------------------- outline accuracy
GO_AST = r'''package main
import ("fmt";"go/ast";"go/parser";"go/token";"os")
func main(){fs:=token.NewFileSet();f,err:=parser.ParseFile(fs,os.Args[1],nil,0);if err!=nil{os.Exit(1)}
for _,d:=range f.Decls{switch x:=d.(type){case *ast.FuncDecl:fmt.Printf("%s %d %d\n",x.Name.Name,fs.Position(x.Pos()).Line,fs.Position(x.End()).Line)
case *ast.GenDecl:for _,s:=range x.Specs{if t,ok:=s.(*ast.TypeSpec);ok{a,b:=x.Pos(),x.End();if x.Lparen.IsValid(){a,b=t.Pos(),t.End()};fmt.Printf("%s %d %d\n",t.Name.Name,fs.Position(a).Line,fs.Position(b).Line)}}}}}
'''
TS_AST = r'''const ts=require(process.argv[2]);const fs=require('fs');const src=fs.readFileSync(process.argv[3],'utf8');
const sf=ts.createSourceFile('x.ts',src,ts.ScriptTarget.Latest,true);const line=p=>sf.getLineAndCharacterOfPosition(p).line+1;
for(const s of sf.statements){let n=null;
 if(ts.isFunctionDeclaration(s)||ts.isClassDeclaration(s)||ts.isInterfaceDeclaration(s)||ts.isTypeAliasDeclaration(s)||ts.isEnumDeclaration(s))n=s.name&&s.name.text;
 else if(ts.isVariableStatement(s)){const d=s.declarationList.declarations[0];if(d.initializer&&(ts.isArrowFunction(d.initializer)||ts.isFunctionExpression(d.initializer)))n=d.name.getText(sf)}
 if(n)console.log(n,line(s.getStart(sf)),line(s.end));}
'''


def outline_accuracy():
    res = {}
    # Go: stdlib sources vs go/ast
    gd = tempfile.mkdtemp()
    open(os.path.join(gd, "main.go"), "w").write(GO_AST)
    code, out, _ = sh("go build -o goast main.go", gd)
    goroot = subprocess.run(["go", "env", "GOROOT"], stdout=subprocess.PIPE, text=True).stdout.strip()
    import glob as _g
    files = sorted(f for f in _g.glob(goroot + "/src/**/*.go", recursive=True) if not f.endswith("_test.go") and "/testdata/" not in f)
    random.Random(5).shuffle(files)
    files = files[:150]
    res["go"] = _compare(files, lambda f: subprocess.run([os.path.join(gd, "goast"), f], stdout=subprocess.PIPE, text=True).stdout)
    # TS: real .ts sources vs the TypeScript compiler
    tsmod = os.path.join(shared("vitest"), "node_modules", "typescript")
    tsfiles = [f for f in __import__("glob").glob(os.path.join(shared("vitest"), "node_modules", "**", "*.ts"), recursive=True)
               if not f.endswith(".d.ts") and "/typescript/" not in f][:150]
    tsfiles += [f for f in __import__("glob").glob(os.path.join(shared("vitest"), "node_modules", "**", "*.d.ts"), recursive=True) if "/typescript/" not in f][:100]
    tsjs = os.path.join(gd, "tsast.js")
    open(tsjs, "w").write(TS_AST)
    res["ts"] = _compare(tsfiles, lambda f: subprocess.run(["node", tsjs, tsmod, f], stdout=subprocess.PIPE, text=True).stdout)
    # Rust: crates in the cargo registry vs syn
    rs_dir = os.path.join(WORK, "_rsast")
    if shutil.which("cargo"):
        if not os.path.exists(os.path.join(rs_dir, "target", "release", "rsast")):
            shutil.copytree(os.path.join(HERE, "tools", "rsast"), rs_dir, dirs_exist_ok=True)
            sh("cargo build -q --release", rs_dir, env=dict(os.environ))
        tool = os.path.join(rs_dir, "target", "release", "rsast")
        rsfiles = sorted(_g.glob(os.path.expanduser("~/.cargo/registry/src/*/*/src/**/*.rs"), recursive=True))
        random.Random(3).shuffle(rsfiles)
        if os.path.exists(tool) and rsfiles:
            res["rs"] = _compare(rsfiles[:200], lambda f: subprocess.run([tool, f], stdout=subprocess.PIPE, text=True).stdout)
    for k, v in res.items():
        print("outline %-3s files %3d · symbols %5d · recall %.1f%% · exact start %.1f%% · end within ±1 %.1f%% · extra %d" % (
            k, v["files"], v["truth"], 100 * v["found"] / max(v["truth"], 1), 100 * v["start"] / max(v["found"], 1), 100 * v["end"] / max(v["found"], 1), v["extra"]))
        for m in v["misses"][:8]:
            print("   miss", m)
        for m in v["wrong_end"][:8]:
            print("   end ", m)
    return res


def _compare(files, truth_fn):
    agg = dict(files=0, truth=0, found=0, start=0, end=0, extra=0, misses=[], wrong_end=[])
    for f in files:
        truth = []
        for l in truth_fn(f).split("\n"):
            p = l.split()
            if len(p) == 3:
                truth.append((p[0], int(p[1]), int(p[2])))
        if not truth:
            continue
        agg["files"] += 1
        try:
            mine = [s for s in nav.outline(f) if s[2] == 0]
        except Exception as e:  # noqa: BLE001
            agg["misses"].append((f, "CRASH %r" % e))
            continue
        byname = {}
        for s in mine:
            byname.setdefault(s[4], []).append(s)
        for name, a, b in truth:
            agg["truth"] += 1
            cands = byname.get(name, [])
            hit = min(cands, key=lambda s: abs(s[0] - a), default=None)
            if not hit or abs(hit[0] - a) > 3:
                agg["misses"].append((os.path.basename(f), name, a))
                continue
            agg["found"] += 1
            agg["start"] += hit[0] == a
            if abs(hit[1] - b) <= 1:
                agg["end"] += 1
            else:
                agg["wrong_end"].append((os.path.basename(f), name, (a, b), (hit[0], hit[1])))
        agg["extra"] += max(0, len(mine) - len(truth))
    return agg


# ---------------------------------------------------------------- report
def report(rows, problems, loop_tot, fz, pf, ol):
    L = ["# lean testbed report", "", "Generated by `python3 testbed/bench.py all` against lean at `%s`." % LEAN_ROOT, ""]
    tot_raw = sum(r["raw"] for r in rows)
    tot_w = sum(r["wrapped"] for r in rows)
    L += ["## Scenarios (%d real captures)" % len(rows), "",
          "Total: **%d → %d tokens (%.1f%% saved)** · problems: **%d** · false successes: **%d**" % (
              tot_raw, tot_w, 100 * (1 - tot_w / max(tot_raw, 1)), len(problems), sum(1 for r in rows if r["false_ok"])), "",
          "| scenario | kind | exit | raw tok | lean tok | saved | facts kept | auto (hook) |", "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        L.append("| %s | %s | %d | %d | %d | %.0f%% | %d/%d | %s %d |" % (r["name"], r["kind"], r["code"], r["raw"], r["wrapped"], 100 * (1 - r["wrapped"] / max(r["raw"], 1)),
                                                                   r["facts"] - len(r["missing"]), r["facts"], r["amode"], r["auto"]))
    if problems:
        L += ["", "### Problems", ""] + ["- **%s**: %s" % (n, "; ".join(b)) for n, b in problems]
    if loop_tot:
        L += ["", "## Fix-loop episodes (real execution, context cost incl. re-reads)", "",
              "raw output %d tok → lean %d tok · context cost %d → %d tok (**%.0f%% saved**)" % (
                  loop_tot[0], loop_tot[1], loop_tot[2], loop_tot[3], 100 * (1 - loop_tot[3] / max(loop_tot[2], 1)))]
    if fz:
        L += ["", "## Robustness", "", "fuzz crashes: %d · outputs grown: %d · slow (>1s): %d" % (len(fz[0]), fz[1], len(fz[2]))]
    if pf:
        L += ["", "## Latency", ""] + ["- %s: median %.0f ms, max %.0f ms" % (k, a, b) for k, (a, b) in pf.items()]
    if ol:
        L += ["", "## Outline accuracy vs compiler", ""] + ["- %s: %d symbols in %d files · recall %.1f%% · exact start %.1f%% · end ±1 %.1f%%" % (
            k, v["truth"], v["files"], 100 * v["found"] / max(v["truth"], 1), 100 * v["start"] / max(v["found"], 1), 100 * v["end"] / max(v["found"], 1)) for k, v in ol.items()]
    with open(os.path.join(HERE, "REPORT.md"), "w") as f:
        f.write("\n".join(L) + "\n")
    print("wrote testbed/REPORT.md")


def main(argv):
    cmd = argv[0] if argv else "eval"
    rest = [a for a in argv[1:] if not a.startswith("-")]
    if cmd == "capture":
        capture(rest, "--force" in argv)
    elif cmd == "eval":
        _, problems = evaluate(rest, "-v" in argv)
        return 1 if problems else 0
    elif cmd == "loop":
        loop()
    elif cmd == "fuzz":
        c, _, _ = fuzz(int(rest[0]) if rest else 400)
        return 1 if c else 0
    elif cmd == "perf":
        perf()
    elif cmd == "outline":
        outline_accuracy()
    elif cmd == "all":
        rows, problems = evaluate()
        lt = loop()
        fz = fuzz()
        pf = perf()
        ol = outline_accuracy()
        report(rows, problems, lt, fz, pf, ol)
        return 1 if problems or fz[0] else 0
    else:
        print(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
