"""Output filter engine.

filter_output(cmd, text, cwd, code) -> (shown, kind)

Rules: success -> one line; failure -> everything that matters; unknown -> near-lossless
generic squeeze. Full raw output is always written to a log whose path is printed whenever
anything was removed, so nothing is unrecoverable.
"""
import json
import os
import re
import time
from . import parse, util

SMALL = 1500          # chars; below this, output passes through (cleaned) untouched
MAX_LINES = 160       # generic budget
LONG_LINE = 400

IMPORTANT = re.compile(r"(?i)(\berr(or)?s?\b|\bERR!|\bfail(ed|ure|s)?\b|\bfatal\b|\bpanic|exception|traceback|\bwarn(ing)?s?\b|cannot |unable to|denied|not found|no such|undefined|\bassert|segfault|killed|timed? ?out|E\d{3,4}\b|TS\d{4})")
ERRORISH = re.compile(r"(?i)(\berror\b|\bfailed\b|\bfatal\b|\bpanic\b|exception|\bERR!)")

# commands whose output IS the content the agent asked for: never touch beyond ANSI cleanup
READ_CMDS = re.compile(r"^(cat|head|tail|sed|nl|bat|less|more|awk|jq|yq|wc|ls|tree|find|fd|file|stat|echo|printf|env|which|type|pwd|diff|cmp|xxd|od|base64|python3? -c|node -e|o|g|t|c|sub|x|lean)\b")
GIT_DIFF = re.compile(r"^git (diff|show|log -p|stash show)")
TEST = re.compile(r"(\bpytest\b|py\.test|-m pytest|\bjest\b|\bvitest\b|\bgo test\b|\bcargo (test|nextest)\b|\b(npm|pnpm|yarn|bun)( run)? test\b|\bmake test\b|\btox\b|\bnox\b|"
                  r"\bmvnw?\b.*\b(test|verify)\b|\bgradlew?\b.*\b(test|check)\b|\brspec\b|\brake test\b|\bruby -I\w* \S*test|\bphpunit\b|\bctest\b|\bdotnet test\b|\bmix test\b)")
BUILD = re.compile(r"(\btsc\b|\beslint\b|\bruff\b|\bmypy\b|\bpyright\b|\bflake8\b|\bpylint\b|\bgo (build|vet)\b|\bgolangci-lint\b|\bcargo (build|check|clippy)\b|\bmake\b|\bgcc\b|\bg\+\+\b|\bclang\b|\b(npm|pnpm|yarn|bun)( run)? (build|lint|typecheck|check|type-check)\b|\bnext build\b|\bvite build\b|\bwebpack\b|\bbiome\b|\bprettier --check\b|\bgradle\b|\bmvn\b)")
INSTALL = re.compile(r"(\b(npm|pnpm|bun) (i|install|ci|add)\b|\byarn( install| add|$)|\bpip3? install\b|\buv (pip install|sync|add|lock)\b|\bpoetry (install|add|lock)\b|\bcargo (fetch|install|update)\b|\bgo (mod (download|tidy)|get|install)\b|\bapt(-get)? (install|update|upgrade)\b|\bbrew (install|upgrade)\b|\bbundle install\b|\bgem install\b|\bcomposer install\b)")
GIT_NET = re.compile(r"^git (push|pull|fetch|clone|checkout|switch|merge|rebase|submodule|lfs)\b")
DOCKER = re.compile(r"^docker( compose| buildx)? (build|pull|push|compose|up)\b")

NOISE = re.compile(r"^(\[INFO\] Download(ing|ed) from |Download(ing|ed) https?://|Progress \(\d+\)|Picked up (JAVA_TOOL_OPTIONS|_JAVA_OPTIONS)|\[INFO\] *$|\[ERROR\] *$|"
                   r"\[ERROR\] (To see the full stack trace|Re-run Maven using|For more information about the errors|\[Help 1\]|-> \[Help 1\]))")
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


def drop_noise(text):
    return "\n".join(l for l in text.split("\n") if not NOISE.match(l))


def generic(text):
    from . import jsonc
    j, info = jsonc.compact(text)
    if j is not None:
        return j + "\n" + jsonc.note(info)
    return "\n".join(budget(squeeze(drop_noise(text).rstrip("\n").split("\n"))))


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
    order = ["failed", "errors", "files_failed", "passed", "skipped", "warnings", "xfailed", "xpassed", "ignored", "todo", "pkgs_ok", "pkgs_failed"]
    lab = {"pkgs_ok": "pkgs ok", "pkgs_failed": "pkgs failed", "files_failed": "files failed"}
    return ", ".join("%d %s" % (c[k], lab.get(k, k)) for k in order if c.get(k))


NUM = re.compile(r"-?\d+(?:\.\d+)?")


def _brace(vals, cap=12):
    u = []
    for v in vals:
        if v not in u:
            u.append(v)
    if len(u) == 1:
        return u[0]
    if len(u) >= 4 and all(re.fullmatch(r"-?\d+", v) for v in u):
        ints = [int(v) for v in u]
        step = ints[1] - ints[0]
        if step and all(ints[i + 1] - ints[i] == step for i in range(len(ints) - 1)):
            return "{%d..%d%s}" % (ints[0], ints[-1], "" if step == 1 else "/%d" % step)
    return "{" + "|".join(u[:cap]) + ("|…+%d" % (len(u) - cap) if len(u) > cap else "") + "}"


def merge_texts(texts):
    """Texts identical except for numbers -> one template with {a|b} slots / {a..b/step} ranges. None if not mergeable."""
    if len(set(texts)) == 1:
        return texts[0]
    skel = [NUM.split(t) for t in texts]
    if any(x != skel[0] for x in skel[1:]):
        return None
    nums = [NUM.findall(t) for t in texts]
    out = skel[0][0]
    for i in range(len(nums[0])):
        out += _brace([n[i] for n in nums]) + skel[0][i + 1]
    return out


BRACE = re.compile(r"\{(-?\d+)\.\.(-?\d+)(?:/(\d+))?\}|\{([^{}]*\|[^{}]*)\}")


def expand_braces(text, cap=400):
    """Inverse of merge_texts / compact_names, line by line: every concrete variant (tests prove lossless-ness)."""
    out = []
    for line in text.split("\n"):
        variants = [line]
        while any(BRACE.search(v) for v in variants) and len(variants) < cap:
            nxt = []
            for v in variants:
                m = BRACE.search(v)
                if not m:
                    nxt.append(v)
                    continue
                if m.group(4) is not None:
                    alts = [a for a in m.group(4).split("|") if not a.startswith("…")]
                else:
                    a, b, st = int(m.group(1)), int(m.group(2)), int(m.group(3) or 1)
                    alts = [str(x) for x in range(a, b + (1 if b >= a else -1), st if b >= a else -st)]
                nxt += [v[:m.start()] + x + v[m.end():] for x in alts]
            variants = nxt
        out += variants
    return "\n".join(out)


def compact_names(names, limit=60):
    """test_x[1], test_x[2] -> test_x[1|2]; case 5 … case 195 -> case {5..195/5}; shared `file::` prefix printed once."""
    groups, order = {}, []
    for n in names:
        m = re.match(r"^(.*?)\[(.*)\]$", n)
        base, param = (m.group(1), m.group(2)) if m else (n, None)
        if base not in groups:
            groups[base] = []
            order.append(base)
        if param is not None:
            groups[base].append(param)
    items = [b + ("[%s]" % "|".join(groups[b]) if groups[b] else "") for b in order]
    tmpl, torder = {}, []
    for it in items:
        k = tuple(NUM.split(it))
        if k not in tmpl:
            tmpl[k] = []
            torder.append(k)
        tmpl[k].append(it)
    items = []
    for k in torder:
        g = tmpl[k]
        items += [merge_texts(g)] if len(g) >= 3 else g
    for sep in ("::", " > "):
        if len(items) > 1 and all(sep in i for i in items):
            pre = items[0].split(sep)[0]
            if all(i.split(sep)[0] == pre for i in items):
                items = [items[0]] + [i[len(pre) + len(sep):] for i in items[1:]]
                break
    more = len(items) - limit
    return ", ".join(items[:limit]) + (" +%d more (see log)" % more if more > 0 else "")


def _pick_detail(d, cap=8):
    """Keep assertion diffs (+/- lines) over context when trimming."""
    if len(d) <= cap:
        return d
    diff = [l for l in d if l[:1] in "+-" and not l.startswith(("- Expected", "+ Received", "+++", "---"))]
    keep = d[:2] + [l for l in diff if l not in d[:2]]
    return keep[:cap + 4]


def _rel(path, cwd):
    try:
        if os.path.isabs(path) and cwd and path.startswith(os.path.abspath(cwd) + os.sep):
            return os.path.relpath(path, cwd)
    except Exception:
        pass
    return path


def render_tests(res, cwd, prev=None, elapsed=None):
    """prev: {name: normalized msg} from the last run of this runner in this repo (or None)."""
    fails = res["failures"]
    now = {f["name"]: _norm(f["msg"]) for f in fails}
    counts = dict(res["counts"])
    if counts.get("files_failed") and not any(f["name"].endswith("(suite)") for f in fails):
        counts.pop("files_failed")      # files that merely contain failing tests: no new information
    head = "%s %s: %s" % ("✗" if fails or counts.get("failed") or res.get("build_failed") else "✓",
                          res["runner"], _count_str(counts) or ("%d failed" % len(fails) if fails else "no tests ran?"))
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
    shown, same = 0, []
    for fs in groups.values():
        f = fs[0]
        names = [x["name"] for x in fs]
        msg = (merge_texts([x["msg"] for x in fs]) or f["msg"]) if len(fs) > 1 else f["msg"]
        if all(n in unchanged for n in names):
            same += names       # message already in context from the previous run
            continue
        if shown >= 15:
            out.append("✗ %s — %s" % (compact_names(names, 4), msg[:120]))
            continue
        shown += 1
        if len(fs) > 1:
            out.append("×%d %s" % (len(fs), msg))
            out.append("   " + compact_names(names))
        else:
            out.append("✗ %s" % f["name"])
            if msg:
                out.append("   " + msg)
        detail = f["detail"]
        if len(fs) > 1 and all(len(x["detail"]) == len(detail) for x in fs):
            detail = [merge_texts([x["detail"][i] for x in fs]) or detail[i] for i in range(len(detail))]
        diff = [d for d in detail if d[:1] in "+-" and not d.startswith(("- Expected", "+ Received"))]
        if diff and all(d[1:].strip().rstrip(",") in msg for d in diff):
            detail = [d for d in detail if d not in diff and not d.startswith(("- Expected", "+ Received"))]   # values already in msg
        for d in _pick_detail(detail):
            out.append("   " + d[:200])
        if f["loc"]:
            out.append("   @ %s:%d" % (_rel(f["loc"][0], cwd), f["loc"][1]))
            out += _snippet(f["loc"], cwd)
    if same:
        out.append("= unchanged: " + compact_names(same, 30))
    w = res.get("warnings") or []
    if w:
        by = {}
        for loc, typ, m in w:
            by.setdefault((typ, _norm(m), loc), []).append(m)
        parts = ["×%d %s: %s @ %s" % (len(ms), typ, merge_texts(ms) or ms[0], _rel(loc, cwd)) for (typ, _, loc), ms in sorted(by.items(), key=lambda kv: -len(kv[1]))[:5]]
        out.append("⚠ " + " · ".join(parts))
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
    elif re.search(r"\b(mvnw?|gradlew?)\b", cmd):
        hint = ["junit"]
    elif "rspec" in cmd:
        hint = ["rspec"]
    for name in hint + [n for n in parse.TEST_PARSERS if n not in hint]:
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


def _locs(ds, max_files=12):
    by = {}
    for d in ds:
        by.setdefault(d["f"], []).append(d["l"])
    parts = ["%s:%s" % (f, _brace([str(x) for x in sorted(set(ls))], cap=40)) for f, ls in list(by.items())[:max_files]]
    return ", ".join(parts) + (" +%d files (see log)" % (len(by) - max_files) if len(by) > max_files else "")


def render_diags(diags, tool="", cwd=None):
    if cwd:
        diags = [dict(d, f=_rel(d["f"], cwd)) for d in diags]
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
        groups.setdefault((d["s"], _norm(d["m"])), []).append(d)
    single = []
    lines = 0
    for _, ds in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        if len(ds) >= 3:
            code = next((d["code"] for d in ds if d["code"]), "")   # rustc names the lint on the first hit only
            msg = merge_texts([d["m"] for d in ds]) or ds[0]["m"]
            out.append("×%d %s%s — %s" % (len(ds), code + " " if code else "", msg[:300], _locs(ds)))
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


def build(text, cmd, code=None, cwd=None):
    diags = parse.diagnostics(text)
    tool = (main_segment(cmd).split() or [""])[0]
    errs = [d for d in diags if d["s"] == "error"]
    clean_run = code == 0 or (code is None and not ERRORISH.search(text))
    if not errs and clean_run:
        if diags:
            return render_diags(diags, tool, cwd).replace("⚠", "✓", 1)
        return "✓ %s ok" % (main_segment(cmd)[:60] or tool)
    if errs:
        body = render_diags(diags, tool, cwd)
        # errors the diagnostic parser could not structure (linker, config) -> keep them
        lines_ = drop_noise(text).split("\n")
        extra = [l for l in lines_ if (ERRORISH.search(l) or re.match(r"^\[ERROR\]\s+(symbol|location):", l)) and not any(d["m"][:40] in l for d in diags)]
        extra = [re.sub(r"^\[ERROR\]\s*", "", l) for l in extra if not re.search(
            r"^(error: (could not compile|aborting)|Found \d+ error|\d+ errors?|\[ERROR\] (COMPILATION ERROR|Failed to execute goal .*Compilation failure))", l.strip())]
        seen = set()
        extra = [l for l in extra if not (l.strip() in seen or seen.add(l.strip()))][:10]
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
            if out is None:                               # e.g. compile error before any test ran
                b = build(text, cmd, code, cwd) if parse.diagnostics(text) else None
                if b is not None and len(b) < len(text):
                    return b, k
        if k == "build":
            out = build(text, cmd, code, cwd)
            if out is not None and len(out) < len(text) * 0.9:
                return out, k
            out = None
        if len(text) < SMALL and k != "diff":
            return text, k
        if k == "install":
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
