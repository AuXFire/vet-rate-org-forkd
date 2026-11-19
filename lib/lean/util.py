"""Shared helpers: paths, cleaning, logs, metrics, per-project state. Stdlib only."""
import hashlib
import json
import os
import re
import subprocess
import time

HOME = os.environ.get("LEAN_HOME") or os.path.expanduser("~/.lean")
DEFAULT_CHARS_PER_TOKEN = 3.6  # conservative for code/logs; `x calibrate` refines it

ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[()][A-Za-z0-9]|\x1b[=>78]")


def path(*parts):
    p = os.path.join(HOME, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def clean(s):
    """Strip ANSI and collapse carriage-return progress redraws (keep last frame)."""
    s = ANSI.sub("", s).replace("\r\n", "\n")
    if "\r" in s:
        s = "\n".join(l.rstrip("\r").rsplit("\r", 1)[-1] for l in s.split("\n"))
    return s.replace("\x08", "")


def short(p):
    h = os.path.expanduser("~")
    return "~" + p[len(h):] if p.startswith(h + os.sep) else p


def cfg():
    try:
        with open(path("config.json")) as f:
            return json.load(f)
    except Exception:
        return {}


def save_cfg(c):
    with open(path("config.json"), "w") as f:
        json.dump(c, f, indent=1)


def chars_per_token():
    return float(cfg().get("chars_per_token") or DEFAULT_CHARS_PER_TOKEN)


def tokens(nchars):
    return int(nchars / chars_per_token())


def save_log(text, tag="out"):
    """Persist full raw output; returns display path. Prunes to the newest 300 logs."""
    h = hashlib.sha1(("%s%s" % (time.time(), len(text))).encode()).hexdigest()[:7]
    p = path("log", "%s-%s.log" % (re.sub(r"[^\w.-]", "", os.path.basename(tag))[:16] or "out", h))
    with open(p, "w", errors="replace") as f:
        f.write(text)
    try:
        d = os.path.dirname(p)
        logs = sorted((os.path.join(d, x) for x in os.listdir(d)), key=os.path.getmtime)
        for old in logs[:-300]:
            os.remove(old)
    except Exception:
        pass
    return short(p)


def metric(kind, raw, shown, **kw):
    """raw/shown are char counts. One JSON line per event; never raises."""
    try:
        rec = {"ts": round(time.time(), 1), "kind": kind, "raw": raw, "shown": shown,
               "sid": kw.pop("sid", None) or os.environ.get("LEAN_SID")}
        rec.update(kw)
        with open(path("metrics.jsonl"), "a") as f:
            f.write(json.dumps(rec, separators=(",", ":")) + "\n")
    except Exception:
        pass


def sh(cmd, cwd=None, timeout=60):
    """Run a short helper command; return (code, stdout) with stderr merged."""
    try:
        r = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str), stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout, text=True, errors="replace")
        return r.returncode, r.stdout
    except Exception as e:
        return 127, str(e)


def root(cwd=None):
    c, out = sh(["git", "rev-parse", "--show-toplevel"], cwd=cwd or os.getcwd(), timeout=5)
    return out.strip() if c == 0 and out.strip() else os.path.abspath(cwd or os.getcwd())


def _state_path(r):
    return path("state", hashlib.sha1(r.encode()).hexdigest()[:12] + ".json")


def load_state(r):
    try:
        with open(_state_path(r)) as f:
            return json.load(f)
    except Exception:
        return {}


def save_state(r, st):
    try:
        st["root"] = r
        with open(_state_path(r), "w") as f:
            json.dump(st, f)
    except Exception:
        pass


def update_state(r, **kw):
    st = load_state(r)
    st.update(kw)
    save_state(r, st)
    return st


QUIET_ENV = {"CI": "1", "NO_COLOR": "1", "FORCE_COLOR": "0", "TERM": "dumb", "PAGER": "cat",
             "GIT_PAGER": "cat", "PYTHONUNBUFFERED": "1", "NPM_CONFIG_FUND": "false",
             "NPM_CONFIG_UPDATE_NOTIFIER": "false", "NPM_CONFIG_PROGRESS": "false",
             "PIP_DISABLE_PIP_VERSION_CHECK": "1", "CARGO_TERM_PROGRESS_WHEN": "never",
             "DOCKER_CLI_HINTS": "false"}


def quiet_env(mild=False):
    """mild=True skips CI/TERM, for arbitrary commands where those could change semantics."""
    e = dict(os.environ)
    e.update({k: v for k, v in QUIET_ENV.items() if not (mild and k in ("CI", "TERM"))})
    return e
