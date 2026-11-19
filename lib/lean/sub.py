"""`sub`: search & replace across files. Dry-run by default (prints a compact diff).

sub PATTERN REPLACEMENT [paths...] [-w] [-r] [-i] [--apply] [-g GLOB]
  default: literal match      -r  regex (Python syntax, \\1 groups)
  -w  whole word              -i  ignore case
  -g  file glob filter (repeatable), e.g. -g '*.ts'
"""
import os
import re
from . import util


def main(args):
    if len(args) < 2 or args[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    flags = {"-w": False, "-r": False, "-i": False, "--apply": False}
    globs, pos, i = [], [], 0
    while i < len(args):
        a = args[i]
        if a in flags:
            flags[a] = True
        elif a == "-g" and i + 1 < len(args):
            globs.append(args[i + 1])
            i += 1
        else:
            pos.append(a)
        i += 1
    pat, rep, paths = pos[0], pos[1], pos[2:] or ["."]
    if not flags["-r"]:   # sed-style escapes in literal mode
        pat, rep = (x.replace("\\n", "\n").replace("\\t", "\t") for x in (pat, rep))
    rx_src = pat if flags["-r"] else re.escape(pat)
    if flags["-w"]:
        rx_src = r"\b%s\b" % rx_src
    try:
        rx = re.compile(rx_src, re.I if flags["-i"] else 0)
    except re.error as e:
        print("sub: bad regex: %s" % e)
        return 2
    if not flags["-r"]:
        rep = rep.replace("\\", "\\\\")
    multi = "\n" in pat or "\\n" in rx_src
    cmd = ["rg", "-l", "--hidden", "-g", "!.git"] + (["-U"] if multi else []) + sum((["-g", g] for g in globs), []) + (["-i"] if flags["-i"] else [])
    cmd += ["-e", rx_src if flags["-r"] or flags["-w"] else pat] + ([] if flags["-r"] or flags["-w"] else ["-F"]) + paths
    c, out = util.sh(cmd, timeout=60)
    files = [f for f in out.split("\n") if f.strip() and os.path.isfile(f)]
    total, shown, report = 0, 0, []
    for f in files:
        try:
            src = open(f, errors="strict").read()
        except Exception:
            continue
        new, n = rx.subn(rep, src)
        if not n:
            continue
        total += n
        report.append("%s (%d)" % (f, n))
        if shown < 60:
            a, b = src.split("\n"), new.split("\n")
            for ln, (x, y) in enumerate(zip(a, b), 1):
                if x != y and shown < 60:
                    report.append("  %d -%s" % (ln, x.strip()[:150]))
                    report.append("  %d +%s" % (ln, y.strip()[:150]))
                    shown += 1
        if flags["--apply"]:
            with open(f, "w") as fh:
                fh.write(new)
    nfiles = len([r for r in report if not r.startswith("  ")])
    if flags["--apply"]:
        from .hook import quick_check
        bad = [e for e in (quick_check(r.rsplit(" (", 1)[0]) for r in report if not r.startswith("  ")) if e]
        if bad:
            report.append("✗ check after apply:\n" + "\n".join(bad))
    if not total:
        print("sub: no matches")
        return 1
    head = "%s %d replacements in %d files" % ("applied" if flags["--apply"] else "dry-run:", total, nfiles)
    print(head + "\n" + "\n".join(report) + ("" if flags["--apply"] else "\n(re-run with --apply to write)"))
    return 0
