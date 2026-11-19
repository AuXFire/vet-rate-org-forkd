"""Run a shell command quietly, log raw output, print the filtered view, preserve exit code."""
import os, signal, subprocess, sys, time
from . import filt, util


def execute(cmd, cwd=None, timeout=None, mild=False):
    t0 = time.time()
    p = subprocess.Popen(cmd, shell=True, cwd=cwd, env=util.quiet_env(mild), stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    chunks = []

    def stop(*_):
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except Exception:
            pass
    import threading
    main = threading.current_thread() is threading.main_thread()
    old = signal.signal(signal.SIGTERM, stop) if main else None
    try:
        out, _ = p.communicate(timeout=timeout)
        chunks.append(out)
    except subprocess.TimeoutExpired:
        stop()
        out, _ = p.communicate()
        chunks.append(out + b"\n[lean: timed out after %ds]" % timeout)
    except KeyboardInterrupt:
        stop()
        out, _ = p.communicate()
        chunks.append(out + b"\n[lean: interrupted]")
    finally:
        if main:
            signal.signal(signal.SIGTERM, old)
    raw = b"".join(chunks).decode("utf-8", "replace")
    return p.returncode if p.returncode is not None else 130, raw, time.time() - t0


def run(cmd, cwd=None, label=None, kind_hint=None, timeout=None, show_cmd=True, mild=False):
    cwd = cwd or os.getcwd()
    code, raw, dt = execute(cmd, cwd, timeout, mild)
    shown, kind = filt.filter_output(cmd, raw, cwd, code=code, elapsed=dt)
    lines = []
    if show_cmd:
        lines.append("$ " + (label or cmd))
    lines.append(shown.rstrip("\n"))
    clean = util.clean(raw)
    log = util.save_log(raw, (label or cmd).split()[0] if (label or cmd).split() else "out")
    if code not in (0, None):     # success needs no footer; `x last` still recovers the raw output
        lines.append("[exit %d · raw %d lines → %s]" % (code, clean.count("\n") + 1, log) if len(shown) < len(clean) * 0.9 else "[exit %d]" % code)
    text = "\n".join(l for l in lines if l is not None)
    print(text)
    util.metric("cmd:" + (kind_hint or kind), len(clean), len(text), cmd=(label or cmd)[:120], code=code, dt=round(dt, 2))
    return code
