"""Output filter engine.

filter_output(cmd, text, cwd, code) -> (shown, kind)

Rules: success -> one line; failure -> everything that matters; unknown -> near-lossless
generic squeeze. Full raw output is always written to a log whose path is printed whenever
anything was removed, so nothing is unrecoverable.
"""
import json, os, re, time
from . import parse, util

SMALL = 1500          # chars; below this, output passes through (cleaned) untouched
MAX_LINES = 160       # generic budget
LONG_LINE = 400

IMPORTANT = re.compile(r"(?i)(\berr(or)?s?\b|\bERR!|\bfail(ed|ure|s)?\b|\bfatal\b|\bpanic|exception|traceback|\bwarn(ing)?s?\b|cannot |unable to|denied|not found|no such|undefined|\bassert|segfault|killed|timed? ?out|E\d{3,4}\b|TS\d{4})")
ERRORISH = re.compile(r"(?i)(\berror\b|\bfailed\b|\bfatal\b|\bpanic\b|exception|\bERR!)")

# commands whose output IS the content the agent asked for: never touch beyond ANSI cleanup
READ_CMDS = re.compile(r"^(cat|head|tail|sed|nl|bat|less|more|awk|jq|yq|wc|ls|tree|find|fd|file|stat|echo|printf|env|which|type|pwd|diff|cmp|xxd|od|base64|python3? -c|node -e|o|g|t|c|sub|x|lean)\b")
GIT_DIFF = re.compile(r"^git (diff|show|log -p|stash show)")
TEST = re.compile(r"(\bpytest\b|py\.test|-m pytest|\bjest\b|\bvitest\b|\bgo test\b|\bcargo (test|nextest)\b|\b(npm|pnpm|yarn|bun)( run)? test\b|\bmake test\b|\btox\b|\bnox\b)")
BUILD = re.compile(r"(\btsc\b|\beslint\b|\bruff\b|\bmypy\b|\bpyright\b|\bflake8\b|\bpylint\b|\bgo (build|vet)\b|\bgolangci-lint\b|\bcargo (build|check|clippy)\b|\bmake\b|\bgcc\b|\bg\+\+\b|\bclang\b|\b(npm|pnpm|yarn|bun)( run)? (build|lint|typecheck|check|type-check)\b|\bnext build\b|\bvite build\b|\bwebpack\b|\bbiome\b|\bprettier --check\b|\bgradle\b|\bmvn\b)")
INSTALL = re.compile(r"(\b(npm|pnpm|bun) (i|install|ci|add)\b|\byarn( install| add|$)|\bpip3? install\b|\buv (pip install|sync|add|lock)\b|\bpoetry (install|add|lock)\b|\bcargo (fetch|install|update)\b|\bgo (mod (download|tidy)|get|install)\b|\bapt(-get)? (install|update|upgrade)\b|\bbrew (install|upgrade)\b|\bbundle install\b|\bgem install\b|\bcomposer install\b)")
GIT_NET = re.compile(r"^git (push|pull|fetch|clone|checkout|switch|merge|rebase|submodule|lfs)\b")
DOCKER = re.compile(r"^docker( compose| buildx)? (build|pull|push|compose|up)\b")

NOISY_GIT = re.compile(r"^(remote: (Enumerating|Counting|Compressing|Total|Resolving)|Enumerating objects|Counting objects|Compressing objects|Writing objects|Total \d+|Delta compression|Resolving deltas|Receiving objects|Unpacking objects|Updating files|remote:\s*$)")
GENERATED = re.compile(r"(package-lock\.json|yarn\.lock|pnpm-lock\.yaml|Cargo\.lock|poetry\.lock|uv\.lock|go\.sum|composer\.lock|Gemfile\.lock|\.min\.(js|css)|\.map$|__snapshots__/|\.snap$|(^|/)dist/|(^|/)build/|\.pb\.go$|_pb2\.py$|\.generated\.)")


def segments(cmd):
    """Meaningful sub-commands of `cd x && FOO=1 npm test 2>&1; echo done`."""
    out = []
    for s in re.split(r"\s*(?:&&|\|\||;|\n)\s*", cmd.strip()):
        s = re.sub(r"^(\s*\w+=\S*\s+)+", "", s.strip())       # env assignments
        s = re.sub(r"^(sudo|time|exec|nice|timeout \d+\w?|uv run|poetry run|pipenv run|npx|bunx|pnpm exec|dotenv --|then|do|else)\s+", "", s)
        s = re.sub(r"\s+\d?>&\d|\s+2>/dev/null", "", s)
        if not s or re.match(r"^(cd|pushd|popd|export|source|\.|set|true|false|echo|printf|mkdir|clear|done|fi|for|while|if|sleep|rm)\b", s):
            continue
        out.append(s)
    return out


def main_segment(cmd):
    """The most significant segment: test > build > install > other (last wins among equals)."""
    segs = segments(cmd)
    for rx in (TEST, BUILD, INSTALL):
        for s in segs:
            if rx.search(s):
                return s
    return segs[-1] if segs else ""


def kind_of(cmd):
    if re.search(r"(^|\s)RAW=1\b", cmd):
        return "raw"
    first = cmd.split("<<")[0]       # heredoc bodies are data, not pipes
    if re.search(r"(?<!\|)\|(?!\|)", first) and not re.search(r"\|\s*(tee|cat)\b", first):
        return "pipe"   # agent already shaped the output
    m = main_segment(cmd.split("<<")[0] if "<<" in cmd else cmd)
    if not m:
        return "generic"
    if GIT_DIFF.match(m):
        return "diff"
    for k, rx in (("test", TEST), ("build", BUILD), ("install", INSTALL)):
        if rx.search(m):
            return k
    if READ_CMDS.match(m):
        return "read"
    if GIT_NET.match(m):
        return "gitnet"
    if DOCKER.match(m):
        return "docker"
    return "generic"


# ---------------------------------------------------------------- generic squeeze
def _norm(l):
    return re.sub(r"0x[0-9a-f]+|\d+", "#", l)


def squeeze(lines):
    """Collapse exact repeats and runs of near-identical lines; cap very long lines."""
    out, i, n = [], 0, len(lines)
    while i < n:
        l = lines[i]
        j = i + 1
        while j < n and lines[j] == l:
            j += 1
        if j - i > 1:
            out.append("%s  (×%d)" % (l, j - i))
            i = j
            continue
        k = _norm(l)
        j = i + 1
        while j < n and _norm(lines[j]) == k and lines[j].strip():
            j += 1
        if j - i >= 4:
            out += [l, "  … %d similar lines …" % (j - i - 2), lines[j - 1]]
            i = j
            continue
        out.append(l)
        i += 1
    res = []
    for l in out:
        if len(l) > LONG_LINE:
            l = l[:300] + " …[+%d chars]" % (len(l) - 300)
        res.append(l)
    # collapse blank runs
    final = []
    for l in res:
        if l.strip() or (final and final[-1].strip()):
            final.append(l)
    return final


def budget(lines, max_lines=MAX_LINES, head=40, tail=60):
    if len(lines) <= max_lines:
        return lines
    mid = range(head, len(lines) - tail)
    keep = set()
    for i in mid:
        if IMPORTANT.search(lines[i]):
            keep.update((i - 1, i, i + 1))
    keep = sorted(k for k in keep if head <= k < len(lines) - tail)[:max_lines - head - tail]
    out, last = lines[:head], head - 1
    for k in keep:
        if k != last + 1:
            out.append("  … %d lines …" % (k - last - 1))
        out.append(lines[k])
        last = k
    if len(lines) - tail != last + 1:
        out.append("  … %d lines …" % (len(lines) - tail - last - 1))
    return out + lines[-tail:]


def maybe_json(text):
    s = text.strip()
    if len(s) < SMALL or s[:1] not in "[{" or "\n " not in s:
        return None
    try:
        return json.dumps(json.loads(s), separators=(",", ":"), ensure_ascii=False)
    except Exception:
        return None


def generic(text):
    j = maybe_json(text)
    if j is not None:
        return j
    return "\n".join(budget(squeeze(text.rstrip("\n").split("\n"))))


# ---------------------------------------------------------------- tests
def _snippet(loc, cwd, ctx=1):
    if not loc:
        return []
    f, ln = loc
    for base in (cwd, util.root(cwd)):
        p = f if os.path.isabs(f) else os.path.join(base or ".", f)
        if os.path.isfile(p):
            try:
                with open(p, errors="replace") as fh:
                    src = fh.read().split("\n")
            except Exception:
                return []
            a, b = max(1, ln - ctx), min(len(src), ln + ctx)
            return ["  %s%4d│ %s" % (">" if i == ln else " ", i, src[i - 1].rstrip()[:160]) for i in range(a, b + 1)]
    if not os.path.isabs(f) and "/" not in f.strip("./"):   # runner printed a path relative to its own root
        c, out = util.sh(["git", "ls-files", "--", "*" + os.path.basename(f)], cwd=cwd, timeout=5)
        hits = [h for h in out.split("\n") if h.strip()] if c == 0 else []
        if len(hits) == 1:
            return _snippet((os.path.join(util.root(cwd), hits[0]), ln), cwd, ctx)
    return []


def _count_str(c):
    order = ["failed", "errors", "files_failed", "passed", "skipped", "xfailed", "xpassed", "ignored", "todo", "pkgs_ok", "pkgs_failed"]
    lab = {"pkgs_ok": "pkgs ok", "pkgs_failed": "pkgs failed", "files_failed": "files failed"}
    return ", ".join("%d %s" % (c[k], lab.get(k, k)) for k in order if c.get(k))


def render_tests(res, cwd, prev=None, elapsed=None):
    """prev: {name: normalized msg} from the last run of this runner in this repo (or None)."""
    fails = res["failures"]
    now = {f["name"]: _norm(f["msg"]) for f in fails}
    head = "%s %s: %s" % ("✗" if fails or res["counts"].get("failed") or res.get("build_failed") else "✓",
                          res["runner"], _count_str(res["counts"]) or ("%d failed" % len(fails) if fails else "no tests ran?"))
    if elapsed:
        head += " (%.1fs)" % elapsed
    unchanged = set()
    if prev is not None:
        fixed = [n for n in prev if n not in now]
        new = [n for n in now if n not in prev]
        still = [n for n in now if n in prev]
        unchanged = {n for n in still if prev[n] == now[n]}
        bits = []
        if fixed:
            bits.append("%d fixed" % len(fixed))
        if still:
            bits.append("%d still" % len(still))
        if new:
            bits.append("%d new" % len(new))
        if bits:
            head += "  [Δ " + " · ".join(bits) + "]"
    out = [head]
    if res.get("build_failed"):
        out.append("build failed: " + ", ".join(res["build_failed"]))
    groups = {}
    for f in fails:
        groups.setdefault(_norm(f["msg"]) or f["name"], []).append(f)
    shown = 0
    for key, fs in groups.items():
        f = fs[0]
        if all(x["name"] in unchanged for x in fs):
            names = ", ".join(x["name"] for x in fs[:6]) + (" +%d" % (len(fs) - 6) if len(fs) > 6 else "")
            out.append("= %s — unchanged: %s" % (names, f["msg"][:120]))
            continue
        if shown >= 15:
            out.append("✗ %s — %s" % (", ".join(x["name"] for x in fs[:4]), f["msg"][:100]))
            continue
        shown += 1
        if len(fs) > 1:
            out.append("×%d %s" % (len(fs), f["msg"]))
            out.append("   " + ", ".join(x["name"] for x in fs[:10]) + (" +%d more" % (len(fs) - 10) if len(fs) > 10 else ""))
        else:
            out.append("✗ %s" % f["name"])
            if f["msg"]:
                out.append("   " + f["msg"])
        for d in f["detail"][:8]:
            out.append("   " + d[:200])
        if f["loc"]:
            out.append("   @ %s:%d" % f["loc"])
            out += _snippet(f["loc"], cwd)
    return "\n".join(out), now


def test_result(text, cmd=""):
    hint = []
    if "vitest" in cmd:
        hint = ["vitest"]
    elif "jest" in cmd:
        hint = ["jest"]
    elif "pytest" in cmd or "tox" in cmd:
        hint = ["pytest"]
    elif "go test" in cmd:
        hint = ["go"]
    elif "cargo" in cmd:
        hint = ["cargo"]
    for name in hint + [n for n in ("pytest", "vitest", "jest", "go", "cargo") if n not in hint]:
        try:
            r = parse.TEST_PARSERS[name](text)
        except Exception:
            r = None
        if r and (r["failures"] or r["counts"]):
            return r
    return None


def tests(text, cwd, cmd="", elapsed=None, code=None):
    r = test_result(text, cmd)
    if not r:
        return None
    if code not in (None, 0) and not r["failures"] and not r["counts"].get("failed"):
        return None   # nonzero exit we couldn't explain: let generic show everything important
    root = util.root(cwd)
    st = util.load_state(root)
    prevall = st.get("tests", {})
    body, now = render_tests(r, cwd, prevall.get(r["runner"]), elapsed)
    prevall[r["runner"]] = now
    st["tests"] = prevall
    st["last_test"] = {"runner": r["runner"], "cmd": cmd, "failed": list(now)[:50], "ts": time.time(),
                       "summary": body.split("\n", 1)[0]}
    util.save_state(root, st)
    if not r["failures"] and not r["counts"].get("failed"):
        # successful run: also surface warnings count, nothing else
        return body
    # failures we couldn't structure (e.g. collection error) -> append important raw lines
    if r["counts"].get("failed", 0) + r["counts"].get("errors", 0) > len(r["failures"]):
        imp = [l for l in text.split("\n") if ERRORISH.search(l)][:15]
        body += "\n" + "\n".join("   " + l.strip()[:200] for l in imp)
    return body


# ---------------------------------------------------------------- diagnostics
DEP_PATH = re.compile(r"(^|/)(node_modules|site-packages|dist-packages|\.cargo/registry|pkg/mod|vendor)/")


def render_diags(diags, tool=""):
    deps = [d for d in diags if DEP_PATH.search(d["f"])]
    diags = [d for d in diags if not DEP_PATH.search(d["f"])] if deps and len(deps) < len(diags) + 1 else diags
    out_dep = None
    if deps:
        by = {}
        for d in deps:
            k = (d["code"], d["m"][:90])
            by[k] = by.get(k, 0) + 1
        top = sorted(by.items(), key=lambda kv: -kv[1])[:3]
        out_dep = "deps: %d issues inside dependencies (often config: skipLibCheck / moduleResolution / versions): %s" % (
            len(deps), "; ".join("×%d %s %s" % (n, c, m) for (c, m), n in top))
    errs = [d for d in diags if d["s"] == "error"]
    warns = [d for d in diags if d["s"] != "error"]
    files = {d["f"] for d in diags}
    out = ["%s %s%d errors, %d warnings in %d files" % ("✗" if errs or (deps and any(d["s"] == "error" for d in deps)) else "⚠", tool + ": " if tool else "", len(errs), len(warns), len(files))]
    if out_dep:
        out.append(out_dep)
    show = errs + (warns if len(warns) <= 12 or not errs else [])
    groups = {}
    for d in show:
        groups.setdefault((d["code"], _norm(d["m"])), []).append(d)
    single = []
    lines = 0
    for (code, _), ds in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        if len(ds) >= 3:
            locs = ", ".join("%s:%d" % (d["f"], d["l"]) for d in ds[:8]) + (" +%d" % (len(ds) - 8) if len(ds) > 8 else "")
            out.append("×%d %s%s — %s" % (len(ds), code + " " if code else "", ds[0]["m"][:160], locs))
            lines += 1
        else:
            single += ds
    byfile = {}
    for d in single:
        byfile.setdefault(d["f"], []).append(d)
    for f, ds in byfile.items():
        if lines > 60:
            out.append("… +%d more issues (see log)" % sum(len(v) for v in byfile.values()))
            break
        out.append(f)
        for d in sorted(ds, key=lambda d: (d["l"], d["c"])):
            out.append("  %d:%d %s%s%s" % (d["l"], d["c"], "" if d["s"] == "error" else "warn ", d["code"] + " " if d["code"] else "", d["m"][:220]))
            lines += 1
    if warns and show is errs:
        by = {}
        for d in warns:
            by[d["code"] or _norm(d["m"])[:40]] = by.get(d["code"] or _norm(d["m"])[:40], 0) + 1
        out.append("warnings: " + ", ".join("×%d %s" % (v, k) for k, v in sorted(by.items(), key=lambda kv: -kv[1])[:8]))
    return "\n".join(out)


def build(text, cmd, code=None):
    diags = parse.diagnostics(text)
    tool = (main_segment(cmd).split() or [""])[0]
    errs = [d for d in diags if d["s"] == "error"]
    clean_run = code == 0 or (code is None and not ERRORISH.search(text))
    if not errs and clean_run:
        if diags:
            return render_diags(diags, tool).replace("⚠", "✓", 1)
        return "✓ %s ok" % (main_segment(cmd)[:60] or tool)
    if errs:
        body = render_diags(diags, tool)
        # errors the diagnostic parser could not structure (linker, config) -> keep them
        extra = [l for l in text.split("\n") if ERRORISH.search(l) and not any(d["m"][:40] in l for d in diags)]
        extra = [l for l in extra if not re.match(r"^(error: (could not compile|aborting)|Found \d+ error|\d+ errors?)", l.strip())][:10]
        if extra:
            body += "\nother:\n" + "\n".join("  " + l.strip()[:220] for l in extra)
        return body
    return None


def install(text, cmd, code=None):
    ok = code == 0 or (code is None and not re.search(r"(?i)(\bERR!|\berror\b|failed|could not|conflict)", text))
    if not ok:
        return None
    keep = [l.strip() for l in text.split("\n") if re.search(
        r"(?i)((added|removed|changed|audited|up to date)\b.*packages?|Successfully installed|vulnerabilit|Resolved \d+|Installed \d+|Prepared \d+|Uninstalled \d+|already satisfied.*$|Done in|Already up to date|newly installed|Setting up)", l)]
    warn = len(re.findall(r"(?im)^\s*(npm )?warn", text))
    seen, ks = set(), []
    for k in keep:
        kk = _norm(k)
        if kk not in seen:
            seen.add(kk)
            ks.append(k[:200])
    line = "✓ %s" % main_segment(cmd)[:60]
    if ks:
        line += ": " + " · ".join(ks[:4]) + (" (+%d)" % (len(ks) - 4) if len(ks) > 4 else "")
    if warn:
        line += " · %d warnings" % warn
    return line


def gitnet(text):
    lines = [l for l in text.split("\n") if not NOISY_GIT.match(l.strip())]
    lines = [l for l in lines if not re.match(r"^remote:\s+(https?://|Create a pull request|To create a merge request|\s*$)", l)]
    return "\n".join(budget(squeeze(lines))) or "✓ done"


def diff(text):
    """Collapse hunks of lockfiles/generated files to a stat line; keep everything else verbatim."""
    out, skip, adds, dels, name = [], False, 0, 0, None

    def flush():
        if skip and name:
            out.append("   [%s: +%d −%d generated/lockfile, hunks omitted]" % (name, adds, dels))
    for l in text.split("\n"):
        if l.startswith("diff --git "):
            flush()
            m = re.match(r"diff --git a/(\S+) b/(\S+)", l)
            name = m.group(2) if m else None
            skip = bool(name and GENERATED.search(name))
            adds = dels = 0
            out.append(l)
            continue
        if skip:
            if l.startswith("+") and not l.startswith("+++"):
                adds += 1
            elif l.startswith("-") and not l.startswith("---"):
                dels += 1
            continue
        out.append(l if len(l) <= LONG_LINE else l[:300] + " …[+%d chars]" % (len(l) - 300))
    flush()
    return "\n".join(out)


def docker(text, code=None):
    if code == 0 or (code is None and not ERRORISH.search(text)):
        tail = [l.strip() for l in text.split("\n") if re.search(r"(naming to|writing image|Successfully (built|tagged)|digest: sha256|Pulled|Started|Created|Running)", l)]
        return "✓ docker: " + " · ".join(tail[-3:]) if tail else None
    return None


# ---------------------------------------------------------------- entry
def filter_output(cmd, text, cwd=None, code=None, elapsed=None):
    cwd = cwd or os.getcwd()
    text = util.clean(text)
    k = kind_of(cmd)
    if k in ("raw", "read"):
        return text, k
    out = None
    try:
        if k == "test":
            out = tests(text, cwd, cmd, elapsed, code)   # always parse: keeps Δ state current
            if out is not None and len(out) < len(text):
                return out, k
        if len(text) < SMALL and k != "diff":
            return text, k
        if k == "build":
            out = build(text, cmd, code)
        elif k == "install":
            out = install(text, cmd, code)
        elif k == "gitnet":
            out = gitnet(text)
        elif k == "docker":
            out = docker(text, code)
        elif k == "diff":
            out = diff(text)
            if len(out) > 40000:
                out = "\n".join(budget(out.split("\n"), 600, 200, 200))
        if out is None and k == "test":
            out = None
    except Exception:
        out = None
    if out is None:
        out = generic(text) if k != "pipe" else "\n".join(squeeze(text.split("\n")))
    return out, k
