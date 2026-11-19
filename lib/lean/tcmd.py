"""`t` (test) and `c` (check) commands."""
import os, re, shlex, subprocess, sys, threading, time
from . import detect, filt, run, util

T_HELP = """t            smart: rerun last failures → if green, full suite; else related-to-changes; else full
t all        full suite          t <args|paths>   runner with args
t -h         this help           RAW=1 t ...      unfiltered output"""


def _q(xs):
    return " ".join(shlex.quote(x) for x in xs)


def _related(tst, d, files):
    """Command limited to tests related to changed files, or None if we can't scope safely."""
    r, cmd = tst["runner"], tst["cmd"]
    rel = [os.path.relpath(f, d) for f in files if f.startswith(d + os.sep) and os.path.exists(f)]
    if r == "vitest":
        src = [f for f in rel if re.search(r"\.[cm]?[jt]sx?$|\.vue$|\.svelte$", f)]
        return "%s related %s" % (cmd.replace(" run", ""), _q(src)) + " --run" if src else None
    if r == "jest":
        src = [f for f in rel if re.search(r"\.[cm]?[jt]sx?$", f)]
        return "%s --findRelatedTests %s --passWithNoTests" % (cmd, _q(src)) if src else None
    if r == "pytest":
        py = [f for f in rel if f.endswith(".py")]
        if not py:
            return None
        c, out = util.sh(["git", "ls-files", "*.py"], cwd=d, timeout=10)
        tracked = set(out.split()) | set(py)
        targets = set()
        for f in py:
            b = os.path.basename(f)
            if b.startswith("test_") or b.endswith("_test.py") or "/tests/" in "/" + f:
                targets.add(f)
                continue
            stem = b[:-3]
            hits = [t for t in tracked if os.path.basename(t) in ("test_%s.py" % stem, "%s_test.py" % stem)]
            if not hits:
                return None          # unmapped source change: the full suite is the safe choice
            targets.update(hits)
        return "%s %s" % (cmd, _q(sorted(targets)))
    if r == "go":
        dirs = sorted({"./" + (os.path.dirname(f) or ".") for f in rel if f.endswith(".go")})
        return "go test %s" % _q(dirs) if dirs else None
    return None


def _rerun(tst, failed):
    r, cmd = tst["runner"], tst["cmd"]
    if r == "pytest":
        return cmd + " --lf"
    if r == "jest":
        return cmd + " --onlyFailures"
    if r == "vitest":
        files = sorted({n.split(" > ")[0].strip() for n in failed if " > " in n})
        return "%s %s" % (cmd, _q(files)) if files else None
    if r == "go":
        tops = sorted({n.split("/")[0] for n in failed if n != "panic"})
        return "go test -run '^(%s)$' ./..." % "|".join(tops) if tops else None
    if r == "cargo":
        return "cargo test -- %s" % _q(failed[:30])
    return None


def t_main(args):
    if args and args[0] in ("-h", "--help"):
        print(T_HELP)
        return 0
    info = detect.detect()
    d = info["dir"]
    if not info["tests"]:
        print("t: no test runner detected in %s (stacks: %s). Run your command directly." % (util.short(d), ",".join(info["stacks"]) or "none"))
        return 2
    code = 0
    for tst in info["tests"]:
        if args and args[0] != "all":
            code |= run.run("%s %s" % (tst["cmd"], _q(args)), cwd=d, kind_hint="test")
            continue
        if args and args[0] == "all":
            code |= run.run(tst["cmd"], cwd=d, label="t all: " + tst["cmd"], kind_hint="test")
            continue
        st = util.load_state(util.root(d))
        prev = st.get("tests", {}).get(tst["runner"]) or {}
        failed = list(prev)
        rr = _rerun(tst, failed) if failed else None
        if rr:
            c = run.run(rr, cwd=d, label="t: rerun %d failure(s) → %s" % (len(failed), rr), kind_hint="test")
            if c != 0:
                code |= c
                continue
            print("all previous failures pass → full suite")
            code |= run.run(tst["cmd"], cwd=d, label="t: " + tst["cmd"], kind_hint="test")
            continue
        rel = _related(tst, d, detect.changed_files(d))
        if rel:
            code |= run.run(rel, cwd=d, label="t: related → " + rel, kind_hint="test")
        else:
            code |= run.run(tst["cmd"], cwd=d, label="t: " + tst["cmd"], kind_hint="test")
    return code


def c_main(args):
    if args and args[0] in ("-h", "--help"):
        print("c            typecheck + lint (+ vet/clippy) for the nearest project, in parallel\nc <name>     only checks whose name matches (tsc, eslint, ruff, mypy, vet, clippy…)")
        return 0
    info = detect.detect()
    checks = [c for c in info["checks"] if not args or any(a in c["name"] for a in args)]
    if not checks:
        print("c: no checks detected in %s (stacks: %s)" % (util.short(info["dir"]), ",".join(info["stacks"]) or "none"))
        return 2
    results = [None] * len(checks)

    def go(i, ck):
        results[i] = run.execute(ck["cmd"], info["dir"])
    t0 = time.time()
    th = [threading.Thread(target=go, args=(i, ck)) for i, ck in enumerate(checks)]
    for x in th:
        x.start()
    for x in th:
        x.join()
    ok, parts, raw_total, code = [], [], 0, 0
    for ck, (rc, raw, dt) in zip(checks, results):
        clean = util.clean(raw)
        raw_total += len(clean)
        diags = filt.parse.diagnostics(clean)
        errs = [x for x in diags if x["s"] == "error"]
        if rc == 0 and not [x for x in errs if not filt.DEP_PATH.search(x["f"])]:
            w = len(diags)
            ok.append(ck["name"] + (" (%d warn)" % w if w else ""))
            if w and w <= 12:
                parts.append(filt.render_diags(diags, ck["name"]))
            continue
        code |= rc or 1
        log = util.save_log(raw, ck["name"].replace(" ", "_"))
        body = filt.render_diags(diags, ck["name"]) if errs else "✗ %s (exit %d)\n%s" % (ck["name"], rc, filt.generic(clean))
        parts.append(body + "\n[log %s]" % log)
    head = ("✓ " if not code else "✗ ") + "c: " + ", ".join([("ok: " + " ".join(ok)) if ok else ""] + ([] if not code else ["failed: " + " ".join(ck["name"] for ck, r in zip(checks, results) if r[0] != 0)])).strip(", ")
    text = head + " (%.1fs)" % (time.time() - t0) + ("\n" + "\n".join(parts) if parts else "")
    print(text)
    util.metric("cmd:check", raw_total, len(text), cmd="c " + " ".join(args), code=code)
    util.update_state(util.root(info["dir"]), last_check={"ok": not code, "summary": head, "ts": time.time()})
    return code
