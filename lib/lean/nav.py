"""`o`: outline files/dirs, show a symbol's definition, list references, look into dependencies.

o <file>            symbols with line ranges (read only the range you need)
o <dir>             compact repo map: files + top-level symbols
o <file> <sym>      print that symbol's body
o def <name>        find definition(s) anywhere in the repo and print body
o refs <name>       references grouped by file
o def pkg.attr      look inside an installed dependency (python / node / go / cargo)
"""
import ast, glob, json, os, re, shutil, subprocess, sys
from . import util

CODE_EXT = {".py", ".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs", ".go", ".rs", ".java", ".kt", ".cs", ".rb", ".php", ".c", ".h", ".cc", ".cpp", ".hpp", ".swift", ".scala", ".vue", ".svelte"}
SKIP_DIRS = {"node_modules", ".git", "dist", "build", "target", ".venv", "venv", "__pycache__", ".next", "vendor", ".tox", "coverage", ".mypy_cache", ".pytest_cache"}

JS = [
    (re.compile(r"^\s*(?:export\s+)?(?:default\s+)?(?:declare\s+)?(?:async\s+)?function\s*\*?\s*(\w+)"), "fn"),
    (re.compile(r"^\s*(?:export\s+)?(?:default\s+)?(?:declare\s+)?(?:abstract\s+)?class\s+(\w+)"), "class"),
    (re.compile(r"^\s*(?:export\s+)?(?:declare\s+)?interface\s+(\w+)"), "interface"),
    (re.compile(r"^\s*(?:export\s+)?(?:declare\s+)?type\s+(\w+)\b[^=]*="), "type"),
    (re.compile(r"^\s*(?:export\s+)?(?:declare\s+)?(?:const\s+)?enum\s+(\w+)"), "enum"),
    (re.compile(r"^\s*(?:export\s+)?(?:const|let|var)\s+(\w+)\s*(?::[^=]+)?=\s*(?:async\s+)?(?:function\b|\([^)]*\)\s*(?::[^=]+)?=>|\w+\s*=>|React\.\w+|styled|create\w*\()"), "fn"),
    (re.compile(r"^\s+(?:public\s+|private\s+|protected\s+|static\s+|readonly\s+|override\s+|async\s+|get\s+|set\s+)*(#?\w+)\s*(?:<[^>]*>)?\([^)]*\)?\s*(?::\s*[^{;]+)?\{\s*$"), "method"),
]
GO = [(re.compile(r"^func\s+(?:\(([^)]*)\)\s*)?(\w+)"), "func"), (re.compile(r"^type\s+(\w+)\s+(\w+)"), "type")]
RS = [(re.compile(r"^\s*(?:pub(?:\([^)]*\))?\s+)?(?:async\s+)?(?:unsafe\s+)?(?:const\s+)?(?:extern\s+\S+\s+)?(fn|struct|enum|trait|impl|mod|type|macro_rules!)\s*(?:<[^{]*?>\s*)?([\w:]+(?:\s+for\s+[\w:<>]+)?)?"), "rs")]
GEN = [(re.compile(r"^\s*(?:public|private|protected|internal|static|final|abstract|override|async|open|data|sealed|\s)*\s*(class|interface|enum|struct|record|object|def|module|fun|func|function)\s+(\w+)"), "gen")]
KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "function", "else", "do", "try", "with", "constructor"}


def _lines(p):
    with open(p, errors="replace") as f:
        return f.read().split("\n")


def _strip(l):
    l = re.sub(r"(\"(\\.|[^\"\\])*\"|'(\\.|[^'\\])*'|`[^`]*`)", '""', l)
    return re.sub(r"//.*$", "", l)


def _brace_end(lines, i):
    depth, opened = 0, False
    for j in range(i, min(len(lines), i + 4000)):
        s = _strip(lines[j])
        if not opened and j > i + 2 and "{" not in s:
            return i + 1
        for ch in s:
            if ch == "{":
                depth += 1
                opened = True
            elif ch == "}":
                depth -= 1
                if opened and depth <= 0:
                    return j + 1
        if not opened and s.rstrip().endswith(";"):
            return j + 1
    return i + 1


def _sig(l):
    s = l.strip().rstrip("{").rstrip()
    return s if len(s) <= 110 else s[:107] + "…"


def outline(p):
    """-> list of (start, end, depth, signature, name)."""
    ext = os.path.splitext(p)[1].lower()
    lines = _lines(p)
    syms = []
    if ext == ".py":
        try:
            tree = ast.parse("\n".join(lines))
        except SyntaxError:
            tree = None
        if tree:
            def walk(nodes, depth):
                for n in nodes:
                    if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                        start = min([d.lineno for d in n.decorator_list] + [n.lineno])
                        syms.append((start, n.end_lineno, depth, _sig(lines[n.lineno - 1]).rstrip(":"), n.name))
                        walk(n.body, depth + 1)
                    elif depth == 0 and isinstance(n, ast.Assign) and len(lines[n.lineno - 1]) < 120 and all(isinstance(t, ast.Name) and t.id.isupper() for t in n.targets):
                        syms.append((n.lineno, n.end_lineno, 0, _sig(lines[n.lineno - 1]), n.targets[0].id))
            walk(tree.body, 0)
            return syms
    if ext in (".md", ".mdx", ".rst"):
        heads = [(i + 1, len(m.group(1)), m.group(2)) for i, l in enumerate(lines) for m in [re.match(r"^(#{1,6})\s+(.+)", l)] if m]
        for k, (ln, lvl, t) in enumerate(heads):
            end = next((h[0] - 1 for h in heads[k + 1:] if h[1] <= lvl), len(lines))
            syms.append((ln, end, lvl - 1, "#" * lvl + " " + t[:100], t))
        return syms
    if ext == ".json":
        try:
            d = json.loads("\n".join(lines))
            if isinstance(d, dict):
                for k in list(d)[:80]:
                    ln = next((i + 1 for i, l in enumerate(lines) if re.match(r'^\s{0,4}"%s"\s*:' % re.escape(k), l)), 0)
                    syms.append((ln, ln, 0, '"%s": %s' % (k, type(d[k]).__name__), k))
        except Exception:
            pass
        return syms
    pats = JS if ext in (".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs", ".vue", ".svelte") else GO if ext == ".go" else RS if ext == ".rs" else GEN
    stack = []   # (end, depth)
    for i, l in enumerate(lines):
        if not l.strip() or l.lstrip().startswith(("//", "*", "/*", "#")) and ext not in (".rb",):
            continue
        for rx, kind in pats:
            m = rx.match(l)
            if not m:
                continue
            name = m.groups()[-1] if kind not in ("rs", "gen") else (m.group(2) or m.group(1))
            if kind == "func" and m.group(2):
                name = m.group(2)
            elif kind == "type" and pats is GO:
                name = m.group(1)
            if not name or name in KEYWORDS:
                continue
            if kind == "method" and not stack:
                continue        # indented call-looking line outside any class/fn
            end = _brace_end(lines, i)
            while stack and stack[-1] < i + 1:
                stack.pop()
            depth = len(stack)
            if kind == "method" and depth == 0:
                continue
            syms.append((i + 1, end, depth, _sig(l), name))
            if end > i + 1:
                stack.append(end)
            break
    return syms


def fmt_outline(p, syms, n_lines, max_depth=3):
    out = ["%s (%d lines)" % (os.path.relpath(p), n_lines)]
    for s, e, d, sig, _ in syms:
        if d > max_depth:
            continue
        rng = "%d-%d" % (s, e) if e > s else str(s)
        out.append("%-10s%s%s" % (rng, "  " * d, sig))
    return "\n".join(out)


def files_under(d):
    c, out = util.sh(["git", "ls-files", "-co", "--exclude-standard", "--", "."], cwd=d, timeout=20)
    if c == 0:
        fs = [os.path.join(d, x) for x in out.split("\n") if x]
    else:
        fs = []
        for r, ds, names in os.walk(d):
            ds[:] = [x for x in ds if x not in SKIP_DIRS and not x.startswith(".")]
            fs += [os.path.join(r, n) for n in names]
    return [f for f in fs if os.path.isfile(f) and not any(part in SKIP_DIRS for part in f.split(os.sep))]


def repo_map(d):
    fs = [f for f in files_under(d) if os.path.splitext(f)[1].lower() in CODE_EXT]
    if len(fs) > 150:
        agg = {}
        for f in fs:
            rel = os.path.relpath(f, d).split(os.sep)
            key = os.sep.join(rel[:2]) if len(rel) > 2 else (rel[0] if len(rel) > 1 else ".")
            a = agg.setdefault(key, [0, 0])
            a[0] += 1
            try:
                a[1] += sum(1 for _ in open(f, "rb"))
            except Exception:
                pass
        out = ["%s: %d code files — by directory (o <dir> to drill in)" % (util.short(d), len(fs))]
        for k, (n, loc) in sorted(agg.items()):
            out.append("  %-40s %4d files %7d lines" % (k + "/", n, loc))
        return "\n".join(out)
    out = []
    for f in sorted(fs):
        try:
            syms = outline(f)
            n = len(_lines(f))
        except Exception:
            continue
        top = [s[4] for s in syms if s[2] == 0][:14]
        out.append("%s (%d)%s" % (os.path.relpath(f, d), n, ": " + ", ".join(top) + (" …" if len([s for s in syms if s[2] == 0]) > 14 else "") if top else ""))
    return "\n".join(out) or "no code files under %s" % d


def body(p, start, end, max_lines=80):
    lines = _lines(p)
    seg = list(range(start, min(end, len(lines)) + 1))
    if len(seg) > max_lines:
        seg = seg[:50] + [None] + seg[-15:]
    out = []
    for i in seg:
        out.append("  … (%d lines; Read %s offset=%d) …" % (end - start + 1, os.path.relpath(p), start + 50) if i is None else "%5d %s" % (i, lines[i - 1]))
    return "\n".join(out)


DEF_RX = r"(\b(def|class|function|func|fn|type|interface|struct|enum|trait|module|object|record|const|let|var|val)\s+(\([^)]*\)\s*)?{n}\b|\b{n}\s*[:=]\s*(async\s+)?(function\b|\([^)]*\)\s*(:[^=]+)?=>)|^\s*(public |private |protected |static |async |override )*{n}\s*\([^)]*\)\s*(:[^{{]+)?\{{)"


def find_defs(name, d):
    if not shutil.which("rg"):
        return []
    rx = DEF_RX.format(n=re.escape(name))
    c, out = util.sh(["rg", "-n", "--no-heading", "-e", rx, "--max-columns", "300", "-g", "!*.min.*", "-g", "!*.map", "."], cwd=d, timeout=30)
    hits = []
    for l in out.split("\n"):
        m = re.match(r"^(.+?):(\d+):", l)
        if m:
            hits.append((os.path.join(d, m.group(1)), int(m.group(2))))
    return hits


def show_defs(name, d, label=None):
    hits = find_defs(name.split(".")[-1], d)
    if not hits:
        return "no definition of %s found under %s" % (name, util.short(d))
    # rank: .d.ts / source over tests
    hits.sort(key=lambda h: ("test" in h[0], not h[0].endswith((".d.ts", ".pyi")) and "node_modules" in h[0], h[0]))
    out, shown = [], 0
    for p, ln in hits:
        if shown >= 4:
            out.append("also: " + ", ".join("%s:%d" % (os.path.relpath(x, d), y) for x, y in hits[shown:shown + 12]))
            break
        try:
            syms = outline(p)
        except Exception:
            syms = []
        sym = next((s for s in syms if s[0] <= ln <= s[0] + 3 and s[4] == name.split(".")[-1]), None) or \
            next((s for s in syms if s[0] == ln), None)
        s, e = (sym[0], sym[1]) if sym else (ln, ln + 15)
        out.append("── %s:%d-%d" % (os.path.relpath(p, d) if not label else p.replace(d, label), s, e))
        out.append(body(p, s, e))
        shown += 1
    return "\n".join(out)


def refs(name, d):
    if not shutil.which("rg"):
        return "rg not installed"
    c, out = util.sh(["rg", "-n", "-w", "--no-heading", "--max-columns", "200", "--max-columns-preview", "-g", "!*.min.*", "-g", "!*.map", "-F", name, "."], cwd=d, timeout=30)
    rows = [l for l in out.split("\n") if l.strip()]
    if not rows:
        return "no references to %s" % name
    by = {}
    for l in rows:
        m = re.match(r"^\./?(.+?):(\d+):(.*)$", l)
        if m:
            by.setdefault(m.group(1), []).append((int(m.group(2)), m.group(3).strip()))
    total = sum(len(v) for v in by.values())
    out = ["%d refs in %d files" % (total, len(by))]
    if total <= 40:
        for f, hs in by.items():
            out.append(f)
            out += ["  %d: %s" % (n, t[:160]) for n, t in hs]
    else:
        for f, hs in sorted(by.items(), key=lambda kv: -len(kv[1])):
            out.append("%s: %s" % (f, ", ".join(str(n) for n, _ in hs[:20]) + (" +%d" % (len(hs) - 20) if len(hs) > 20 else "")))
    return "\n".join(out)


def dep_dir(pkg, cwd):
    """Locate an installed dependency's source directory."""
    r = util.root(cwd)
    for base in (cwd, r):
        p = os.path.join(base, "node_modules", pkg)
        if os.path.isdir(p):
            return p
    py = shutil.which("python3") or sys.executable
    c, out = util.sh([py, "-c", "import importlib.util,sys,os;s=importlib.util.find_spec(sys.argv[1]);print((s.submodule_search_locations or [os.path.dirname(s.origin)])[0])", pkg], cwd=cwd, timeout=15)
    if c == 0 and os.path.isdir(out.strip()):
        return out.strip()
    if "/" in pkg or os.path.exists(os.path.join(r, "go.mod")):
        c, out = util.sh(["go", "list", "-f", "{{.Dir}}", pkg], cwd=cwd, timeout=30)
        if c == 0 and os.path.isdir(out.strip()):
            return out.strip()
    reg = sorted(glob.glob(os.path.expanduser("~/.cargo/registry/src/*/%s-[0-9]*" % pkg.replace("-", "[-_]"))))
    if reg:
        return reg[-1]
    return None


def main(args):
    if not args or args[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    cwd = os.getcwd()
    if args[0] in ("def", "refs") and len(args) >= 2:
        name = args[1]
        d = os.path.abspath(args[2]) if len(args) > 2 else util.root(cwd)
        if args[0] == "refs":
            print(refs(name, d))
            return 0
        if re.match(r"^[@\w./-]+\.\w+$", name) and not os.path.exists(name) and not find_defs(name.rsplit(".", 1)[-1], d)[:1]:
            pkg, attr = name.rsplit(".", 1)
            dd = dep_dir(pkg, cwd)
            if not dd:
                print("dependency %s not found (node_modules / python / go / cargo)" % pkg)
                return 1
            print(show_defs(attr, dd, label="<%s>" % pkg))
            return 0
        print(show_defs(name, d))
        return 0
    p = args[0]
    if os.path.isdir(p):
        print(repo_map(os.path.abspath(p)))
        return 0
    if not os.path.isfile(p):
        if re.match(r"^[@\w./-]+\.\w+$", p):
            return main(["def", p])
        print("o: no such file or directory: %s" % p)
        return 1
    syms = outline(p)
    if len(args) > 1:
        want = args[1]
        hit = [s for s in syms if s[4] == want or s[4].endswith("." + want)]
        if not hit:
            print("symbol %s not in %s; symbols: %s" % (want, p, ", ".join(s[4] for s in syms[:40])))
            return 1
        for s in hit[:3]:
            print("── %s:%d-%d" % (p, s[0], s[1]))
            print(body(p, s[0], s[1], max_lines=200))
        return 0
    n = len(_lines(p))
    print(fmt_outline(p, syms, n) if syms else "%s (%d lines): no symbols recognised" % (p, n))
    return 0
