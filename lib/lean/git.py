"""`g`: compact git views.

g | g s          one-line status: branch, ahead/behind, counts, changed files
g d [args]       diffstat + tight diff (-U1), lockfile/generated hunks collapsed
g l [n] [args]   short log (default 15)
g <anything>     plain git passthrough with quiet env (output filtered)
"""
import os, re
from . import filt, run, util

G = ["git", "-c", "color.ui=never", "-c", "core.pager=cat"]


def _g(*a):
    return util.sh(G + list(a), timeout=60)


def status():
    c, out = _g("status", "--porcelain=v2", "--branch", "--untracked-files=all")
    if c != 0:
        return out.strip()
    br, ab, files, cnt = "?", "", [], {}
    for l in out.split("\n"):
        if l.startswith("# branch.head"):
            br = l.split()[-1]
        elif l.startswith("# branch.ab"):
            a, b = l.split()[2:4]
            ab = (" ↑" + a[1:] if a != "+0" else "") + (" ↓" + b[1:] if b != "-0" else "")
        elif l.startswith("# branch.upstream"):
            pass
        elif l[:2] in ("1 ", "2 ", "u "):
            n = {"1": 8, "2": 9, "u": 10}[l[0]]
            parts = l.split(" ", n)
            xy, path = parts[1], parts[n].split("\t")[0]
            if l[0] == "u":
                code = "U"
            else:
                code = xy[1] if xy[1] != "." else xy[0]
            cnt[code] = cnt.get(code, 0) + 1
            files.append("%s%s %s" % (code, "+" if xy[0] not in ".U" and l[0] != "u" else " ", path))
        elif l.startswith("? "):
            cnt["?"] = cnt.get("?", 0) + 1
            files.append("?  " + l[2:])
    head = "%s%s · %s" % (br, ab, " ".join("%s%d" % (k, v) for k, v in sorted(cnt.items())) or "clean")
    if len(files) > 25:
        files = files[:25] + ["… +%d more" % (len(files) - 25)]
    return head + ("\n" + "\n".join(files) if files else "")


def diff(args):
    staged = [a for a in args if a in ("--cached", "--staged")]
    c, stat = _g("diff", "--stat=120,80", *args)
    c2, body = _g("diff", "-U1", "--no-ext-diff", *args)
    if c2 != 0:
        return body.strip()
    if not body.strip():
        c3, st = _g("diff", "--cached", "--stat=120,80") if not staged else (0, "")
        return "no unstaged changes" + (" (staged:\n%s)" % st.rstrip() if st.strip() else "")
    shown = filt.diff(body)
    if len(shown) > 60000:
        shown = "\n".join(filt.budget(shown.split("\n"), 900, 300, 300))
    return stat.rstrip() + "\n" + shown.rstrip()


def log(args):
    n = "15"
    if args and args[0].isdigit():
        n, args = args[0], args[1:]
    c, out = _g("log", "-n", n, "--format=%h %ad %s%d", "--date=short", *args)
    return out.rstrip()


def main(args):
    if args and args[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    sub = args[0] if args else "s"
    rest = args[1:]
    if sub in ("s", "st", "status") and not rest:
        out = status()
    elif sub in ("d", "diff"):
        out = diff(rest)
    elif sub in ("l", "log") and not any(a in ("-p", "--patch", "--stat") for a in rest):
        out = log(rest)
    else:
        return run.run("git " + " ".join(_quote(a) for a in args), show_cmd=False)
    print(out)
    util.metric("cmd:git", len(out), len(out), cmd="g " + " ".join(args))
    return 0


def _quote(a):
    import shlex
    return shlex.quote(a)
