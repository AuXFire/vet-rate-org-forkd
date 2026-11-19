"""Parsers: test-runner output -> structured failures; compiler/linter output -> diagnostics.

Every parser is best-effort and returns None when it doesn't recognise the output, so callers
fall back to the generic filter (nothing is ever dropped silently: raw output is always logged).
"""
import re

COUNT = re.compile(r"(\d+) (failed|passed|skipped|errors?|xfailed|xpassed|deselected|ignored|todo|pending)")


def _counts(line):
    out = {}
    for n, k in COUNT.findall(line):
        k = {"error": "errors"}.get(k, k)
        out[k] = out.get(k, 0) + int(n)
    return out


def _fail(name, msg, loc=None, detail=None):
    return {"name": name, "msg": (msg or "").strip()[:300], "loc": loc, "detail": detail or []}


# ---------------------------------------------------------------- pytest
PY_FRAME = re.compile(r"^(\S+\.py):(\d+): ?(\w*)")


def pytest(text):
    if not re.search(r"(=+ (FAILURES|ERRORS|short test summary info) =+|=+ .*\b(passed|failed|error|no tests ran)\b.* in [\d.]+s)", text):
        return None
    lines = text.split("\n")
    res = {"runner": "pytest", "counts": {}, "failures": []}
    for l in reversed(lines):
        if re.match(r"^=+ .* in [\d.]+s", l) or re.match(r"^=+ no tests ran", l):
            res["counts"] = _counts(l)
            break
    # sections
    sections, cur, name, mode = {}, None, None, None
    for l in lines:
        m = re.match(r"^=+ (FAILURES|ERRORS|warnings summary|short test summary info|.* in [\d.]+s.*) =+$", l)
        if m:
            mode = m.group(1)
            name = None
            continue
        if mode in ("FAILURES", "ERRORS"):
            h = re.match(r"^_{3,} (.+?) _{3,}$", l)
            if h:
                name = h.group(1)
                cur = sections.setdefault(name, [])
                continue
            if name is not None:
                cur.append(l)
    summary = [l for l in lines if re.match(r"^(FAILED|ERROR) \S", l)]
    seen = set()
    for s in summary:
        m = re.match(r"^(FAILED|ERROR) (\S+)(?: - (.*))?$", s)
        if not m:
            continue
        nodeid, msg = m.group(2), m.group(3) or ""
        short = nodeid.split("::")[-1]
        key = None
        for k in sections:
            kk = k.replace("ERROR collecting ", "").replace("ERROR at setup of ", "").replace("ERROR at teardown of ", "")
            if kk == short or kk.endswith("." + short) or kk == nodeid or nodeid.endswith(kk.replace(".", "::")):
                key = k
                break
        body = sections.get(key, []) if key else []
        seen.add(key)
        res["failures"].append(_py_failure(nodeid, msg, body, m.group(1) == "ERROR"))
    for k, body in sections.items():  # sections without summary line (e.g. -rN)
        if k not in seen:
            res["failures"].append(_py_failure(k, "", body, k.startswith("ERROR")))
    return res


def _py_failure(nodeid, msg, body, is_error):
    elines = [l[1:].strip() for l in body if l.startswith("E ")]
    frames = [PY_FRAME.match(l) for l in body]
    frames = [(f.group(1), int(f.group(2))) for f in frames if f]
    proj = [f for f in frames if "site-packages" not in f[0] and "/lib/python" not in f[0]]
    loc = (proj or frames or [None])[-1]
    if not msg and elines:
        msg = elines[0]
    if not msg:
        tail = [l.strip() for l in body if l.strip()]
        msg = tail[-1] if tail else ""
    detail = elines[:12] if elines else [l.strip() for l in body if l.strip()][-6:]
    if detail and detail[0].strip() == msg.strip():
        detail = detail[1:]
    f = _fail(nodeid, msg, loc, detail)
    f["error"] = is_error
    return f


# ---------------------------------------------------------------- jest
def jest(text):
    if not re.search(r"^Tests:\s+.*total", text, re.M) and "●" not in text:
        return None
    lines = text.split("\n")
    res = {"runner": "jest", "counts": {}, "failures": []}
    for l in lines:
        if l.startswith("Tests:"):
            res["counts"] = _counts(l)
    blocks, cur = [], None
    for l in lines:
        m = re.match(r"^\s{2}● (.+)$", l)
        if m and "Console" not in m.group(1):
            cur = [m.group(1).strip()]
            blocks.append(cur)
            continue
        if cur is not None:
            if re.match(r"^(PASS|FAIL) |^Test Suites:|^Tests:", l):
                cur = None
                continue
            cur.append(l)
    for b in blocks:
        name, body = b[0], [x for x in b[1:]]
        stack = [re.search(r"\(?([^\s()]+\.[cm]?[jt]sx?):(\d+):\d+\)?$", x) for x in body if x.strip().startswith("at ")]
        stack = [(s.group(1), int(s.group(2))) for s in stack if s and "node_modules" not in s.group(1)]
        text_lines = [x.strip() for x in body if x.strip() and not x.strip().startswith("at ")
                      and not re.match(r"^\s*\|?\s*\^", x) and not re.match(r"^\s*>?\s*\d+ \|", x)]
        msg = text_lines[0] if text_lines else ""
        via = ["via " + ", ".join("%s:%d" % fr for fr in stack[1:3])] if len(stack) > 1 else []
        res["failures"].append(_fail(name, msg, stack[0] if stack else None, text_lines[1:12] + via))
    return res


# ---------------------------------------------------------------- vitest
def vitest(text):
    if not re.search(r"^\s*Tests?\s+\d+ (failed|passed)", text, re.M) and not re.search(r"^\s*(FAIL|×|✗)\s+\S+.*>", text, re.M):
        return None
    lines = text.split("\n")
    res = {"runner": "vitest", "counts": {}, "failures": []}
    for l in lines:
        m = re.match(r"^\s*Tests\s+(.*)$", l)
        if m:
            res["counts"].update(_counts(m.group(1)))
        m = re.match(r"^\s*Test Files\s+(\d+) failed", l)
        if m:
            res["counts"]["files_failed"] = int(m.group(1))
    cur = None
    for l in lines:
        m = re.match(r"^\s*FAIL\s+(\S+ > .+|\S+)(?:\s+\[ .* \])?\s*$", l)
        if m:
            cur = {"name": m.group(1).strip() + ("" if " > " in m.group(1) else " (suite)"), "body": []}
            res["failures"].append(cur)
            continue
        if cur is not None:
            if re.match(r"^\s*⎯{3,}", l) or re.match(r"^\s*(Test Files|Tests)\s", l):
                cur = None
                continue
            cur["body"].append(l)
    out = []
    for f in res["failures"]:
        body = f["body"]
        frames = []
        for x in body:
            m = re.search(r"❯\s+(?:[\w.$<>]+\s+)?(\S+?):(\d+):\d+", x)
            if m and "node_modules" not in m.group(1) and (m.group(1), int(m.group(2))) not in frames:
                frames.append((m.group(1), int(m.group(2))))
        tl = [x.strip() for x in body if x.strip() and "❯" not in x and not re.match(r"^\s*\d+\|", x) and not re.match(r"^\s*\|\s*\^", x)]
        detail = tl[1:12] + (["via " + ", ".join("%s:%d" % fr for fr in frames[1:3])] if len(frames) > 1 else [])
        out.append(_fail(f["name"], tl[0] if tl else "", frames[0] if frames else None, detail))
    res["failures"] = out
    return res


# ---------------------------------------------------------------- go test
def gotest(text):
    if not re.search(r"^(--- FAIL|ok\s+\S+|FAIL\s+\S+|PASS$|\?\s+\S+\s+\[no test files\])", text, re.M):
        return None
    lines = text.split("\n")
    res = {"runner": "go", "counts": {}, "failures": []}
    ok = len([l for l in lines if re.match(r"^ok\s+\S+", l)])
    fpk = [l for l in lines if re.match(r"^FAIL\s+\S+", l)]
    cur, depth = None, 0
    for l in lines:
        m = re.match(r"^(\s*)--- FAIL: (\S+)", l)
        if m:
            cur = _fail(m.group(2), "", None, [])
            depth = len(m.group(1))
            res["failures"].append(cur)
            continue
        if cur is not None:
            if re.match(r"^(\s*)--- (PASS|SKIP)", l) or re.match(r"^(FAIL|ok|PASS)\b", l) or re.match(r"^=== ", l):
                cur = None
                continue
            s = l.strip()
            if s:
                fm = re.match(r"^(\S+\.go):(\d+): ?(.*)$", s)
                if fm and not cur["loc"]:
                    cur["loc"] = (fm.group(1), int(fm.group(2)))
                    cur["msg"] = fm.group(3)[:300]
                else:
                    cur["detail"].append(s)
    # parents of failed subtests carry no message; drop them when a child exists
    names = [f["name"] for f in res["failures"]]
    res["failures"] = [f for f in res["failures"] if not (not f["msg"] and any(n.startswith(f["name"] + "/") for n in names))]
    for f in res["failures"]:
        f["detail"] = f["detail"][:10]
        if not f["msg"] and f["detail"]:
            f["msg"] = f["detail"].pop(0)
    # panics / build failures
    for i, l in enumerate(lines):
        if l.startswith("panic: "):
            fr = [x.strip() for x in lines[i:i + 40] if re.match(r"^\s+\S+\.go:\d+", x) and "/go/src/" not in x and "/pkg/mod/" not in x]
            loc = None
            if fr:
                m = re.match(r"(\S+\.go):(\d+)", fr[0])
                loc = (m.group(1), int(m.group(2))) if m else None
            res["failures"].append(_fail("panic", l, loc, fr[:4]))
    res["counts"] = {"pkgs_ok": ok, "pkgs_failed": len(fpk), "failed": len(res["failures"])}
    res["build_failed"] = [l.split()[1] for l in fpk if "[build failed]" in l or "[setup failed]" in l]
    return res


# ---------------------------------------------------------------- cargo test
def cargo_test(text):
    if not re.search(r"^test result: ", text, re.M) and not re.search(r"^running \d+ tests?", text, re.M):
        return None
    lines = text.split("\n")
    res = {"runner": "cargo", "counts": {}, "failures": []}
    for l in lines:
        if l.startswith("test result:"):
            for k, v in _counts(l.replace(";", "")).items():
                res["counts"][k] = res["counts"].get(k, 0) + v
    cur = None
    for l in lines:
        m = re.match(r"^---- (\S+) stdout ----$", l)
        if m:
            cur = _fail(m.group(1), "", None, [])
            res["failures"].append(cur)
            continue
        if cur is not None:
            if l.strip() == "failures:" or l.startswith("test result:") or l.startswith("---- "):
                cur = None
                continue
            s = l.strip()
            if not s or s.startswith(("note: run with `RUST_BACKTRACE", "note: Some details are omitted", "stack backtrace:")):
                continue
            if re.match(r"^(\d+: |at \S+:\d+)", s):
                continue        # backtrace frames: the panic location + snippet already pinpoint it
            pm = re.match(r"^thread '.*?'(?: \(\d+\))? panicked at (\S+?):(\d+):\d+:?(.*)$", s)
            if pm:
                cur["loc"] = (pm.group(1), int(pm.group(2)))
                if pm.group(3).strip():
                    cur["msg"] = pm.group(3).strip()
                continue
            if not cur["msg"]:
                cur["msg"] = s
            else:
                cur["detail"].append(s)
    for f in res["failures"]:
        f["detail"] = f["detail"][:10]
    return res


TEST_PARSERS = {"pytest": pytest, "jest": jest, "vitest": vitest, "go": gotest, "cargo": cargo_test}


# ---------------------------------------------------------------- diagnostics
DIAG = [
    # tsc --pretty false: src/a.ts(12,5): error TS2322: msg
    re.compile(r"^(?P<f>[^\s(][^(]*?)\((?P<l>\d+),(?P<c>\d+)\): (?P<s>error|warning) (?P<code>TS\d+): (?P<m>.+)$"),
    # tsc pretty: src/a.ts:12:5 - error TS2322: msg
    re.compile(r"^(?P<f>\S+?):(?P<l>\d+):(?P<c>\d+) - (?P<s>error|warning) (?P<code>TS\d+): (?P<m>.+)$"),
    # mypy: a.py:3: error: msg  [code]
    re.compile(r"^(?P<f>\S+\.pyi?):(?P<l>\d+):(?:(?P<c>\d+):)? (?P<s>error|warning|note): (?P<m>.+?)(?:\s+\[(?P<code>[\w-]+)\])?$"),
    # ruff concise / flake8 / eslint unix / gcc / go: file:line:col: [sev:] [CODE] msg
    re.compile(r"^(?P<f>[^\s:]+\.\w+):(?P<l>\d+):(?P<c>\d+): (?:(?P<s>error|warning|note|fatal error)(?:\[(?P<rc>[\w:]+)\])?:\s*)?(?:(?P<code>[A-Z]{1,4}\d{2,5})\s+)?(?P<m>.+)$"),
    # go vet / build without col: file:line: msg
    re.compile(r"^(?P<f>[^\s:]+\.go):(?P<l>\d+): (?P<m>.+)$"),
]
RUST_HEAD = re.compile(r"^(?:(?P<s>error|warning)(?:\[(?P<code>E\d+|[\w:]+)\])?: |(?P<code2>[A-Z]{1,4}\d{2,5})(?: \[\*\])? )(?P<m>.+)$")
RUST_LOC = re.compile(r"^\s*--> (?P<f>[^:]+):(?P<l>\d+):(?P<c>\d+)")
ESLINT_FILE = re.compile(r"^(/|\./|[A-Za-z]:\\|\w)[^\s:]*\.(js|jsx|ts|tsx|mjs|cjs|vue|svelte)$")
ESLINT_ROW = re.compile(r"^\s+(?P<l>\d+):(?P<c>\d+)\s+(?P<s>error|warning)\s+(?P<m>.+?)\s{2,}(?P<code>[\w@/-]+)$")


def diagnostics(text):
    out, lines, i = [], text.split("\n"), 0
    efile = None
    while i < len(lines):
        l = lines[i]
        m = RUST_HEAD.match(l)
        if m and not m.group("m").startswith(("aborting", "could not compile", "build failed")) and not re.match(r"^\d+ (warnings?|errors?) emitted", m.group("m")) and "generated" not in m.group("m"):
            for j in range(i + 1, min(i + 5, len(lines))):
                lm = RUST_LOC.match(lines[j])
                if lm:
                    out.append(dict(f=lm.group("f"), l=int(lm.group("l")), c=int(lm.group("c")), s=m.group("s") or "error",
                                    code=m.group("code") or m.group("code2") or "", m=m.group("m")))
                    break
            i += 1
            continue
        if ESLINT_FILE.match(l.strip()) and not l.startswith(" "):
            efile = l.strip()
            i += 1
            continue
        em = ESLINT_ROW.match(l)
        if em and efile:
            out.append(dict(f=efile, l=int(em.group("l")), c=int(em.group("c")), s=em.group("s"), code=em.group("code"), m=em.group("m")))
            i += 1
            continue
        for rx in DIAG:
            d = rx.match(l)
            if d:
                g = d.groupdict()
                sev = (g.get("s") or "error").replace("fatal error", "error")
                if sev == "note":
                    break
                out.append(dict(f=g["f"], l=int(g["l"]), c=int(g.get("c") or 0), s=sev, code=g.get("code") or g.get("rc") or "", m=g["m"].strip()))
                break
        i += 1
    # de-dup exact repeats (tsc/cargo can print twice across passes)
    seen, uniq = set(), []
    for d in out:
        k = (d["f"], d["l"], d["c"], d["m"])
        if k not in seen:
            seen.add(k)
            uniq.append(d)
    return uniq
