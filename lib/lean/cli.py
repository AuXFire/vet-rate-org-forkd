"""Dispatch: t c o g sub -> tools; x / lean -> utilities."""
import sys

X_HELP = """x stats [session|today|week|all]   token usage + measured savings (API $ and plan-limit view)
x waste [n]                        biggest unfiltered outputs in your transcripts (what to fix next)
x run <cmd...>                     run any command through the filter (log + compact view)
x last [n]                         raw output of the most recent (or n-th most recent) filtered command
x brief                            the session-start project brief
x calibrate                        fit chars-per-token from your own transcripts
x config [key value]               show/set config (billing=api|subscription, read_guard_lines, …)
x hook | x statusline              internal: Claude Code hook / status line entry points"""


def main(name, args):
    try:
        if name == "t":
            from . import tcmd
            return tcmd.t_main(args)
        if name == "c":
            from . import tcmd
            return tcmd.c_main(args)
        if name == "o":
            from . import nav
            return nav.main(args)
        if name == "g":
            from . import git
            return git.main(args)
        if name == "sub":
            from . import sub
            return sub.main(args)
        return x(args)
    except BrokenPipeError:
        return 0
    except KeyboardInterrupt:
        return 130


def x(args):
    sub, rest = (args[0], args[1:]) if args else ("-h", [])
    if sub == "hook":
        from . import hook
        return hook.main()
    if sub in ("statusline", "status"):
        from . import usage
        return usage.statusline()
    if sub == "stats":
        from . import usage
        return usage.stats(rest)
    if sub == "waste":
        from . import usage
        return usage.waste(rest)
    if sub == "calibrate":
        from . import usage
        return usage.calibrate()
    if sub == "brief":
        from . import hook
        print(hook.brief(__import__("os").getcwd()))
        return 0
    if sub == "last":
        import os
        from . import util
        d = util.path("log", "x")[:-2]
        logs = sorted((os.path.join(d, f) for f in os.listdir(d)), key=os.path.getmtime) if os.path.isdir(d) else []
        k = int(rest[0]) if rest and rest[0].isdigit() else 1
        if len(logs) < k:
            print("x last: no logs yet")
            return 1
        p = logs[-k]
        print("# " + util.short(p))
        with open(p, errors="replace") as f:
            sys.stdout.write(f.read())
        return 0
    if sub == "run" and rest:
        from . import run
        return run.run(" ".join(rest), show_cmd=False, mild=True)
    if sub == "config":
        from . import util
        c = util.cfg()
        if len(rest) >= 2:
            v = rest[1]
            c[rest[0]] = int(v) if v.isdigit() else (v == "true" if v in ("true", "false") else v)
            util.save_cfg(c)
        print(c or "{} (defaults)")
        return 0
    print(X_HELP)
    return 0
