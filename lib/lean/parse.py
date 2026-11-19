"""Parsers: test-runner output -> structured failures; compiler/linter output -> diagnostics.

Every parser is best-effort and returns None when it doesn't recognise the output, so callers
fall back to the generic filter (nothing is ever dropped silently: raw output is always logged).
"""
import re

COUNT = re.compile(r"(\d+) (failed|passed|skipped|errors?|xfailed|xpassed|deselected|ignored|todo|pending|warnings?)")


def _counts(line):
    out = {}
    for n, k in COUNT.findall(line):
        k = {"error": "errors", "warning": "warnings"}.get(k, k)
        out[k] = out.get(k, 0) + int(n)
    return out


def _uniq(xs):
    out = []
    for x in xs:
        if x not in out:
            out.append(x)
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
    res["warnings"] = []
    wmode = False
    for l in lines:
        if re.match(r"^=+ warnings summary =+$", l):
            wmode = True
            continue
        if wmode and re.match(r"^(=+ |-- Docs:)", l):
            wmode = False
        if wmode:
            wm = re.match(r"^\s+(\S+?\.py):(\d+): (\w*Warning): (.*)$", l)
            if wm:
                res["warnings"].append(("%s:%s" % (wm.group(1), wm.group(2)), wm.group(3), wm.group(4).strip()))
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
GO_STD = re.compile(r"/go[\d.]*/src/|/pkg/mod/|/usr/(local|lib)/go|/testing/testing\.go|/runtime/")


def _go_line(cur, s):
    if s.startswith("panic: ") or s.startswith("panic("):
        if not cur["msg"].startswith("panic"):
            cur["msg"] = s.replace(" [recovered]", "")[:300]
            cur["loc"] = None
        return
    if s.startswith(("goroutine ", "[signal ", "created by ", "exit status ")) or re.match(r"^[\w./*()-]+\(.*\)$", s):
        return                      # stack noise: function-call lines, goroutine headers
    fm = re.match(r"^(\S+\.go):(\d+)(?: \+0x[0-9a-f]+|: ?(.*))?$", s)
    if fm:
        if GO_STD.search(fm.group(1)):
            return
        if not cur["loc"]:
            cur["loc"] = (fm.group(1), int(fm.group(2)))
            if fm.group(3) is not None and not cur["msg"]:
                cur["msg"] = fm.group(3)[:300]
            return
        if fm.group(3) is None:
            return                  # further stack frames
    cur["detail"].append(s)


def gotest(text):
    if not re.search(r"^(--- FAIL|ok\s+\S+|FAIL\s+\S+|PASS$|\?\s+\S+\s+\[no test files\])", text, re.M):
        return None
    lines = text.split("\n")
    res = {"runner": "go", "counts": {}, "failures": []}
    ok = len([l for l in lines if re.match(r"^ok\s+\S+", l)])
    fpk = [l for l in lines if re.match(r"^FAIL\s+\S+", l)]
    buf, running, cur = {}, None, None
    for l in lines:
        mr = re.match(r"^=== (?:RUN|CONT|PAUSE|NAME)\s+(\S+)", l)
        if mr:
            running, cur = mr.group(1), None
            buf.setdefault(running, [])
            continue
        m = re.match(r"^\s*--- FAIL: (\S+)", l)
        if m:
            cur = _fail(m.group(1), "", None, [])
            for x in buf.get(m.group(1), []):      # -v: messages were printed under === RUN
                _go_line(cur, x)
            res["failures"].append(cur)
            continue
        if re.match(r"^\s*--- (PASS|SKIP)", l) or re.match(r"^(FAIL|ok|PASS)\b", l):
            cur = None
            continue
        st = l.strip()
        if not st:
            continue
        if cur is not None:
            _go_line(cur, st)
        elif running is not None and l.startswith((" ", "\t")):
            buf[running].append(st)
    names = [f["name"] for f in res["failures"]]
    res["failures"] = [f for f in res["failures"] if not (not f["msg"] and any(n.startswith(f["name"] + "/") for n in names))]
    for f in res["failures"]:
        f["detail"] = f["detail"][:10]
        if not f["msg"] and f["detail"]:
            f["msg"] = f["detail"].pop(0)
    if not any(f["msg"].startswith("panic") for f in res["failures"]):
        for i, l in enumerate(lines):          # panic outside any test (init, TestMain)
            if l.startswith("panic: "):
                cur = _fail("panic", "", None, [])
                for x in lines[i:i + 60]:
                    if x.strip():
                        _go_line(cur, x.strip())
                cur["detail"] = cur["detail"][:4]
                res["failures"].append(cur)
                break
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
    stop = re.search(r"^error: test failed, to rerun pass `([^`]+)`", text, re.M)
    if stop:
        res["note"] = "cargo stopped after this target (%s): later test targets did not run (use --no-fail-fast)" % stop.group(1)
    return res



# ---------------------------------------------------------------- JUnit (maven surefire / gradle)
JAVA_LIB = re.compile(r"^(java\.base/|java\.|javax\.|jdk\.|sun\.|org\.junit|junit\.|org\.opentest4j|org\.apache\.maven|org\.gradle|worker\.org\.gradle|kotlin\.|org\.testng)")


def junit(text):
    if not re.search(r"Tests run: \d+, Failures: \d+|<<< (FAILURE|ERROR)!|\d+ tests completed, \d+ failed", text):
        return None
    lines = text.split("\n")
    res = {"runner": "junit", "counts": {}, "failures": []}
    for l in lines:
        m = re.search(r"Tests run: (\d+), Failures: (\d+), Errors: (\d+), Skipped: (\d+)", l)
        if m and not re.search(r" in \S+$", l):          # the aggregate line (per-class lines end with "-- in Class")
            run, f, e, sk = map(int, m.groups())
            res["counts"] = {"failed": f, "errors": e, "passed": run - f - e - sk, "skipped": sk}
        g = re.search(r"(\d+) tests completed, (\d+) failed(?:, (\d+) skipped)?", l)
        if g:
            res["counts"] = {"failed": int(g.group(2)), "passed": int(g.group(1)) - int(g.group(2)), "skipped": int(g.group(3) or 0)}
    cur = None
    for l in lines:
        m = re.match(r"^\[ERROR\] (\S.*?) -- Time elapsed: .*<<< (FAILURE|ERROR)!", l) or \
            re.match(r"^(\S.* > .*) FAILED$", l)                                    # gradle
        if m:
            cur = _fail(re.sub(r"^\w+(\.\w+)*\.(?=\w+\.\w+)", "", m.group(1)).strip(), "", None, [])
            cur["_frames"] = []
            res["failures"].append(cur)
            continue
        if cur is None:
            continue
        st = l.strip()
        if not st or st.startswith("[INFO]") or re.match(r"^\[ERROR\] (Tests run|Failures:|Errors:)", st):
            if st.startswith("[") or not st:
                cur = None if st.startswith("[") else cur
            continue
        fm = re.match(r"^at ([\w$./]+)\.[\w$<>]+\(([\w$]+\.(?:java|kt|scala|groovy)):(\d+)\)", st) or \
            re.match(r"^at ([\w$./]+)\(([\w$]+\.(?:java|kt|scala|groovy)):(\d+)\)", st)
        if fm:
            if not JAVA_LIB.match(fm.group(1)):
                cur["_frames"].append((fm.group(2), int(fm.group(3))))
            continue
        if st.startswith(("at ", "...")) or re.match(r"^\.\.\. \d+ more", st):
            continue
        if not cur["msg"]:
            cur["msg"] = re.sub(r"^(org\.opentest4j|java\.lang|org\.junit\.\w+|junit\.framework)\.", "", st)[:300]
        elif len(cur["detail"]) < 6:
            cur["detail"].append(st)
    for f in res["failures"]:
        fr = _uniq(f.pop("_frames", []))
        if fr:
            f["loc"] = fr[0]
            if len(fr) > 1:
                f["detail"].append("via " + ", ".join("%s:%d" % x for x in fr[1:3]))
    return res


def junit_xml_result(cwd, max_age=None):
    """Full result (counts + failures) from fresh JUnit XML reports, or None."""
    import glob
    import os
    import time
    import xml.etree.ElementTree as ET
    max_age = max_age or int(os.environ.get("LEAN_REPORT_MAX_AGE", 900))
    files = set()
    for p in ("build/test-results/**/*.xml", "target/surefire-reports/*.xml", "**/build/test-results/**/*.xml", "**/target/surefire-reports/*.xml"):
        files |= {f for f in glob.glob(os.path.join(cwd, p), recursive=True) if time.time() - os.path.getmtime(f) < max_age}
    if not files:
        return None
    t = f = e = sk = 0
    for x in files:
        try:
            r = ET.parse(x).getroot()
        except Exception:
            continue
        for ts in ([r] if r.tag == "testsuite" else r.iter("testsuite")):
            t += int(ts.get("tests", 0))
            f += int(ts.get("failures", 0))
            e += int(ts.get("errors", 0))
            sk += int(ts.get("skipped", 0))
    fails = junit_xml(cwd, max_age) if f + e else []
    return {"runner": "junit", "counts": {"failed": f, "errors": e, "passed": t - f - e - sk, "skipped": sk}, "failures": fails}


def junit_xml(cwd, max_age=None):
    """Failures from JUnit XML reports (gradle build/test-results, maven surefire-reports) written in the last few minutes."""
    import glob
    import os
    import time
    import xml.etree.ElementTree as ET
    max_age = max_age or int(os.environ.get("LEAN_REPORT_MAX_AGE", 900))
    pats = ["build/test-results/**/*.xml", "target/surefire-reports/*.xml", "**/build/test-results/**/*.xml", "**/target/surefire-reports/*.xml"]
    files = []
    for p in pats:
        files += [f for f in glob.glob(os.path.join(cwd, p), recursive=True) if time.time() - os.path.getmtime(f) < max_age]
    out = []
    for f in sorted(set(files))[:200]:
        try:
            root = ET.parse(f).getroot()
        except Exception:
            continue
        for tc in root.iter("testcase"):
            bad = tc.find("failure")
            if bad is None:
                bad = tc.find("error")
            if bad is None:
                continue
            frames, method = [], None
            cls = tc.get("classname", "")
            for m in re.finditer(r"at (?:[\w.-]*//)?([\w$./]+)\.([\w$<>]+)\(([\w$]+\.(?:java|kt|scala|groovy)):(\d+)\)", bad.text or ""):
                if not JAVA_LIB.match(m.group(1)):
                    frames.append((m.group(3), int(m.group(4))))
                    if m.group(1) == cls and not method:
                        method = m.group(2)
            frames = _uniq(frames)
            msg = (bad.get("message") or (bad.text or "").strip().split("\n")[0])
            typ = (bad.get("type") or "").split(".")[-1]
            msg = re.sub(r"^(org\.opentest4j|java\.lang|org\.junit\.\w+|junit\.framework)\.", "", msg if typ and typ in msg else "%s: %s" % (typ, msg) if typ else msg)
            name = tc.get("name", "")
            if method and not name.startswith(method):
                name = "%s%s" % (method, name if name.startswith("[") else "(%s)" % name)   # parametrized display names
            fl = _fail("%s.%s" % (cls.split(".")[-1], name), msg[:300],
                       frames[0] if frames else None, ["via " + ", ".join("%s:%d" % x for x in frames[1:3])] if len(frames) > 1 else [])
            out.append(fl)
    return out


# ---------------------------------------------------------------- rspec
def rspec(text):
    if not re.search(r"^\d+ examples?, \d+ failures?", text, re.M):
        return None
    lines = text.split("\n")
    res = {"runner": "rspec", "counts": {}, "failures": []}
    for l in lines:
        m = re.match(r"^(\d+) examples?, (\d+) failures?(?:, (\d+) pending)?", l)
        if m:
            n, f, pnd = int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)
            res["counts"] = {"failed": f, "passed": n - f - pnd, "pending": pnd}
    cur = None
    for l in lines:
        m = re.match(r"^  (\d+)\) (.+)$", l)
        if m:
            cur = _fail(m.group(2).strip(), "", None, [])
            cur["_frames"], cur["_code"] = [], None
            res["failures"].append(cur)
            continue
        if cur is None:
            continue
        if re.match(r"^(Finished in|Failed examples:|\d+ examples?)", l):
            cur = None
            continue
        st = l.strip()
        if not st or st == "(compared using ==)" or st == "(compared using eql?)":
            continue
        fm = re.match(r"^# (\S+?):(\d+)(?::in|$)", st)
        if fm:
            if "/gems/" not in fm.group(1) and "/ruby/" not in fm.group(1):
                cur["_frames"].append((fm.group(1).lstrip("./"), int(fm.group(2))))
            continue
        if st.startswith("Failure/Error:"):
            cur["_code"] = st
            continue
        if not cur["msg"]:
            cur["msg"] = st
        elif cur["msg"].endswith(":") and len(cur["detail"]) == 0 and not cur["msg"].startswith("expected"):
            cur["msg"] += " " + st          # "KeyError:" + "key not found: :name"
        else:
            cur["detail"].append(st)
    for f in res["failures"]:
        fr = _uniq(f.pop("_frames", []))
        f.pop("_code", None)
        if f["msg"].startswith("expected") and f["detail"]:
            f["msg"] = "%s, %s" % (f["msg"], f["detail"].pop(0))     # "expected: 7, got: 8"
        if fr:
            f["loc"] = fr[0]
            if len(fr) > 1:
                f["detail"].append("via " + ", ".join("%s:%d" % x for x in fr[1:3]))
        f["detail"] = f["detail"][:8]
    return res


# ---------------------------------------------------------------- minitest
def minitest(text):
    if not re.search(r"^\d+ runs, \d+ assertions, \d+ failures, \d+ errors", text, re.M):
        return None
    lines = text.split("\n")
    res = {"runner": "minitest", "counts": {}, "failures": []}
    for l in lines:
        m = re.match(r"^(\d+) runs, \d+ assertions, (\d+) failures, (\d+) errors, (\d+) skips", l)
        if m:
            n, f, e, sk = map(int, m.groups())
            res["counts"] = {"failed": f, "errors": e, "passed": n - f - e - sk, "skipped": sk}
    i = 0
    while i < len(lines):
        if re.match(r"^\s+\d+\) (Failure|Error):$", lines[i]) and i + 1 < len(lines):
            hm = re.match(r"^(\S+?#\S+?)(?: \[(\S+?):(\d+)\])?:$", lines[i + 1].strip())
            if hm:
                f = _fail(hm.group(1), "", (hm.group(2), int(hm.group(3))) if hm.group(2) else None, [])
                j = i + 2
                frames = []
                while j < len(lines) and lines[j].strip() and not re.match(r"^\s+\d+\) (Failure|Error):$", lines[j]) and not re.match(r"^\d+ runs,", lines[j]):
                    st = lines[j].strip()
                    fm = re.match(r"^(\S+?\.rb):(\d+):in", st)
                    if fm:
                        if "/gems/" not in fm.group(1):
                            frames.append((fm.group(1), int(fm.group(2))))
                    elif not f["msg"]:
                        f["msg"] = st
                    else:
                        f["detail"].append(st)
                    j += 1
                frames = _uniq(frames)
                if frames and not f["loc"]:
                    f["loc"] = frames[0]
                    if len(frames) > 1:
                        f["detail"].append("via " + ", ".join("%s:%d" % x for x in frames[1:3]))
                if f["msg"].startswith("Expected:") and f["detail"] and f["detail"][0].startswith("Actual:"):
                    f["msg"] += ", " + f["detail"].pop(0)
                res["failures"].append(f)
                i = j
                continue
        i += 1
    return res


TEST_PARSERS = {"pytest": pytest, "jest": jest, "vitest": vitest, "go": gotest, "cargo": cargo_test,
                "junit": junit, "rspec": rspec, "minitest": minitest}


# ---------------------------------------------------------------- diagnostics
DIAG = [
    # pyright: /p/a.py:3:12 - error: msg (reportX)
    re.compile(r"^\s*(?P<f>\S+\.pyi?):(?P<l>\d+):(?P<c>\d+) - (?P<s>error|warning): (?P<m>.+?)(?: \((?P<code>report\w+)\))?$"),
    # javac via maven: [ERROR] /p/Calc.java:[3,58] cannot find symbol
    re.compile(r"^\[(?P<s>ERROR|WARNING)\] (?P<f>\S+\.(?:java|kt|scala)):\[(?P<l>\d+),(?P<c>\d+)\] (?P<m>.+)$"),
    # javac / kotlinc: Calc.java:3: error: cannot find symbol
    re.compile(r"^(?P<f>\S+\.(?:java|kt|scala)):(?P<l>\d+): (?P<s>error|warning): (?P<m>.+)$"),
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
ESLINT_ROW = re.compile(r"^\s+(?P<l>\d+):(?P<c>\d+)\s+(?P<s>error|warning)\s+(?P<m>.+?)(?:\s{2,}(?P<code>[\w@/-]+))?\s*$")


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
                    code = m.group("code") or m.group("code2") or ""
                    if not code:    # clippy / rustc lint name lives in the note lines below
                        for k in range(j, min(j + 25, len(lines))):
                            if not lines[k].strip() or RUST_HEAD.match(lines[k]):
                                break
                            nm = re.search(r"clippy::(\w+)|#\[(?:warn|deny)\(([\w:]+)\)\]", lines[k])
                            if nm:
                                code = nm.group(1) or nm.group(2)
                                break
                    out.append(dict(f=lm.group("f"), l=int(lm.group("l")), c=int(lm.group("c")), s=m.group("s") or "error",
                                    code=code, m=m.group("m")))
                    break
            i += 1
            continue
        if ESLINT_FILE.match(l.strip()) and not l.startswith(" "):
            efile = l.strip()
            i += 1
            continue
        em = ESLINT_ROW.match(l)
        if em and efile:
            out.append(dict(f=efile, l=int(em.group("l")), c=int(em.group("c")), s=em.group("s"), code=em.group("code") or "", m=em.group("m")))
            i += 1
            continue
        for rx in DIAG:
            d = rx.match(l)
            if d:
                g = d.groupdict()
                sev = (g.get("s") or "error").lower().replace("fatal error", "error")
                if sev == "note":
                    break
                msg, code = g["m"].strip(), g.get("code") or g.get("rc") or ""
                if not code and i + 1 < len(lines):
                    cm = re.match(r"^\s{2,}.*\((report\w+)\)\s*$", lines[i + 1])
                    if cm:
                        code = cm.group(1)
                wm = re.search(r"\s+\[(-W[\w=-]+)\]$", msg)        # gcc/clang: "... [-Wunused-variable]"
                if wm and not code:
                    msg, code = msg[:wm.start()], wm.group(1)
                out.append(dict(f=g["f"], l=int(g["l"]), c=int(g.get("c") or 0), s=sev, code=code, m=msg))
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
