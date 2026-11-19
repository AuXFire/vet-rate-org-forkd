"""Independent ground truth: extract must-keep facts from RAW output, written separately from lean's parsers.

present(fact, shown) accepts lean's legitimate compressions only:
  - parametrized ids collapsed:  test_x[6]  ⟶  test_x[6|7|8]
  - shared prefix printed once:  tests/a.py::test_b  ⟶  test_b  (when tests/a.py:: appears elsewhere)
"""
import re

ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


def present(fact, shown, _expanded=None):
    if fact in shown:
        return True
    if _expanded is None and "{" in shown:
        from lean.filt import expand_braces, expand_paths
        extra = expand_braces(shown)
        if re.match(r"^.*\(\d+(?: paths)?\):", shown):
            extra += "\n" + "\n".join(expand_paths(shown))
        return present(fact, shown + "\n" + extra, _expanded=True)
    m = re.match(r"^([\w./-]+\.\w+):(\d+)$", fact)
    if m:   # file:line shown grouped by file:  "path\n  line:col …"
        path, line = m.groups()
        cur = None
        for l in shown.split("\n"):
            if not l.startswith(" "):
                cur = l.strip()
            elif cur and (cur == path or cur.endswith("/" + path)) and re.match(r"^\s+%s:" % line, l):
                return True
    m = re.match(r"^(.*)\[([^\]]*)\]$", fact)
    if m:
        base, param = m.groups()
        for g in re.finditer(re.escape(base.split("::")[-1]) + r"\[([^\]]*)\]", shown):
            if param in g.group(1).split("|"):
                return True
    for sep in ("::", " > ", " › "):
        if sep in fact:
            tail = fact.split(sep)[-1]
            head = fact[: -len(tail) - len(sep)]
            if tail in shown and (head.split(sep)[0] in shown or not head):
                return True
            if present(tail, shown) and head.split(sep)[0].split("/")[-1] in shown:
                return True
    return False


def facts(sc, raw):
    raw = ANSI.sub("", raw).replace("\r\n", "\n")
    out = set()
    if sc["expect"] != "fail":
        m = re.findall(r"(\d+) passed", raw)
        return ["%s passed" % m[-1]] if m and "pytest" in sc["cmd"] else []
    for m in re.finditer(r"^(?:FAILED|ERROR) (\S+?)(?: - |$)", raw, re.M):          # pytest summary
        out.add(m.group(1))
    for m in re.finditer(r"^\s*(?:FAIL|×)\s+(\S+\.[jt]sx? > .+?)(?:\s+\d+ms)?$", raw, re.M):   # vitest
        out.add(m.group(1).strip())
    for m in re.finditer(r"^\s+● (.+?)$", raw, re.M):                                # jest
        if "Console" not in m.group(1):
            out.add(m.group(1).split(" › ")[-1].strip())
    for m in re.finditer(r"--- FAIL: (\S+)", raw):                                   # go
        out.add(m.group(1).split("/")[0])
    in_fail = False
    for l in raw.split("\n"):                                                        # cargo
        if l.strip() == "failures:":
            in_fail = True
            continue
        if in_fail and re.match(r"^    \S+$", l):
            out.add(l.strip())
        elif in_fail and l.startswith("test result"):
            in_fail = False
    for m in re.finditer(r"\[ERROR\]\s+(\w+)\.(\w+):\d+", raw):                      # maven surefire
        out.add("%s.%s" % m.groups())
    for m in re.finditer(r"^rspec \./\S+:\d+ # .*? (.+)$", raw, re.M):              # rspec
        out.add(m.group(1).strip())
    for m in re.finditer(r"^\s*\d+\) (?:Failure|Error):\n(\w+)#(\w+)", raw, re.M):  # minitest
        out.add(m.group(2))
    for m in re.finditer(r"error\[(E\d{4})\]", raw):                                 # rustc codes
        out.add(m.group(1))
    for m in re.finditer(r"error (TS\d{4})", raw):                                   # tsc codes
        out.add(m.group(1))
    for m in re.finditer(r"^([\w./-]+\.(?:go|c|h|java)):(\d+):(?:\d+:)? (?:error: )?(.+)$", raw, re.M):  # go/gcc/javac
        if "warning" not in m.group(3) and "note:" not in m.group(3):
            out.add("%s:%s" % (m.group(1).lstrip("./"), m.group(2)))
    return sorted(out)
