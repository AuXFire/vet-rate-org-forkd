"""Token usage + savings: status line, `x stats`, `x waste`, `x calibrate`.

Actual usage comes from Claude Code transcripts (per-request `usage`, de-duplicated by message id;
the format is internal, so parsing is defensive) with the status line's own documented fields as a
fallback. Savings come from lean's metrics (raw vs shown chars, measured at the hook).

Carried savings: text kept out of the context is not re-sent on every later request. For an event
before n later requests (until the next compaction) the counterfactual cost is
  tokens × (cache-write rate + (n−1) × cache-read rate)
because Claude Code writes new context to the prompt cache once and reads it on each later call.
"""
import datetime as dt, glob, json, os, re, sys, time
from . import util

# $/MTok: (input, output, cache_read). Cache writes: 5m = 1.25×input, 1h = 2×input.
# Source: Claude API pricing as of 2026-11 (edit ~/.lean/config.json "prices" to override).
PRICES = [
    ("fable-5-2", (8, 40, 0.20)), ("mythos-5-2", (8, 40, 0.20)),
    ("fable-5-1", (10, 50, 0.25)), ("mythos-5-1", (10, 50, 0.25)),
    ("fable-5", (10, 50, 1.00)), ("mythos-5", (10, 50, 1.00)),
    ("opus-5-5", (4, 20, 0.20)), ("opus-5", (5, 25, 0.50)), ("opus-4-8", (5, 25, 0.50)), ("opus-4-7", (5, 25, 0.50)),
    ("opus-4-6", (5, 25, 0.50)), ("opus-4-5", (5, 25, 0.50)), ("opus-4", (15, 75, 1.50)),
    ("sonnet-5-5", (2, 10, 0.10)), ("sonnet-5", (2, 10, 0.20)), ("sonnet-4", (3, 15, 0.30)),
    ("haiku-5-5", (0.10, 0.50, 0.01)), ("haiku-4-5", (1, 5, 0.10)), ("haiku", (1, 5, 0.10)),
]
DEFAULT_PRICE = (4, 20, 0.20)


def price(model):
    over = util.cfg().get("prices", {})
    m = (model or "").lower()
    for k, v in over.items():
        if k in m:
            return tuple(v)
    for k, v in PRICES:
        if k in m:
            return v
    return DEFAULT_PRICE


def projects_dir():
    return os.environ.get("CLAUDE_PROJECTS_DIR") or os.path.expanduser("~/.claude/projects")


def _ts(s):
    try:
        return dt.datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except Exception:
        return None


# ---------------------------------------------------------------- transcript parsing (incremental)
def parse_transcript(p, cache=True):
    """-> {'req': {msg_id: [ts, model, in, cw5, cw1h, cr, out, speed]}, 'compacts': [ts...], 'sid'}"""
    cp = util.path("cache", "tx", re.sub(r"[^\w.-]", "_", os.path.relpath(p, projects_dir()) if p.startswith(projects_dir()) else p) + ".json")
    st = {"off": 0, "req": {}, "compacts": [], "size": 0}
    if cache:
        try:
            with open(cp) as f:
                st = json.load(f)
        except Exception:
            pass
    try:
        size = os.path.getsize(p)
    except OSError:
        return st
    if size < st.get("off", 0):
        st = {"off": 0, "req": {}, "compacts": []}
    with open(p, "rb") as f:
        f.seek(st["off"])
        data = f.read()
    end = data.rfind(b"\n") + 1
    for line in data[:end].split(b"\n"):
        if not line.strip():
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        t = d.get("type")
        if t == "assistant":
            m = d.get("message") or {}
            u = m.get("usage")
            if not isinstance(u, dict):
                continue
            mid = m.get("id") or d.get("requestId") or d.get("uuid")
            cc = u.get("cache_creation") or {}
            cw1 = cc.get("ephemeral_1h_input_tokens")
            cwt = u.get("cache_creation_input_tokens") or 0
            cw1 = cw1 if isinstance(cw1, int) else 0
            cw5 = max(0, cwt - cw1)
            row = [_ts(d.get("timestamp", "")) or time.time(), m.get("model"), u.get("input_tokens") or 0, cw5, cw1,
                   u.get("cache_read_input_tokens") or 0, u.get("output_tokens") or 0, u.get("speed") or ""]
            old = st["req"].get(mid)
            if not old or row[6] >= old[6]:
                st["req"][mid] = row
        elif t == "system" and d.get("subtype") in ("compact_boundary", "compaction"):
            st["compacts"].append(_ts(d.get("timestamp", "")) or time.time())
    st["off"] += end
    if cache:
        try:
            with open(cp, "w") as f:
                json.dump(st, f)
        except Exception:
            pass
    return st


def cost_of(row):
    _, model, i, cw5, cw1, cr, o, speed = row
    pi, po, pcr = price(model)
    mult = 2 if speed == "fast" else 1
    return mult * (i * pi + cw5 * pi * 1.25 + cw1 * pi * 2 + cr * pcr + o * po) / 1e6


def summarize(reqs):
    s = {"n": 0, "in": 0, "cw": 0, "cr": 0, "out": 0, "cost": 0.0, "models": {}}
    for r in reqs:
        s["n"] += 1
        s["in"] += r[2]
        s["cw"] += r[3] + r[4]
        s["cr"] += r[5]
        s["out"] += r[6]
        c = cost_of(r)
        s["cost"] += c
        s["models"][r[1]] = s["models"].get(r[1], 0) + c
    s["total_in"] = s["in"] + s["cw"] + s["cr"]
    return s


def session_files(sid=None, transcript=None):
    if transcript:
        base = transcript[:-6] if transcript.endswith(".jsonl") else transcript
        return [transcript] + sorted(glob.glob(os.path.join(base, "subagents", "*.jsonl")))
    if sid:
        return sorted(glob.glob(os.path.join(projects_dir(), "*", sid + ".jsonl")) + glob.glob(os.path.join(projects_dir(), "*", sid, "subagents", "*.jsonl")))
    return []


# ---------------------------------------------------------------- savings
def load_metrics(sid=None, since=0):
    p = util.path("metrics.jsonl")
    out = []
    try:
        with open(p) as f:
            for l in f:
                try:
                    d = json.loads(l)
                except Exception:
                    continue
                if d.get("ts", 0) < since or (sid and d.get("sid") != sid):
                    continue
                out.append(d)
    except Exception:
        pass
    return out


def savings(metrics, req_rows, compacts=()):
    """-> dict(direct_tok, carried_tok, usd, by_kind). req_rows sorted by ts (one session) or [] (direct only)."""
    ts_list = sorted(r[0] for r in req_rows)
    comp = sorted(compacts)
    model = req_rows[-1][1] if req_rows else None
    pi, po, pcr = price(model)
    res = {"direct": 0, "carried": 0, "usd": 0.0, "by": {}, "missed": 0, "missed_n": 0}
    import bisect
    for m in metrics:
        if m.get("kind", "").startswith("missed:"):
            res["missed"] += util.tokens(max(0, m.get("raw", 0) - m.get("could", m.get("raw", 0))))
            res["missed_n"] += 1
            continue
        tok = util.tokens(m.get("raw", 0) - m.get("shown", 0))
        t = m.get("ts", 0)
        if ts_list:
            stop = next((c for c in comp if c > t), float("inf"))
            n = bisect.bisect_left(ts_list, stop) - bisect.bisect_right(ts_list, t)
            n = max(n, 1 if tok else 0)
        else:
            n = 1
        carried = tok * n
        usd = tok * (pi * 2 + max(0, n - 1) * pcr) / 1e6
        res["direct"] += tok
        res["carried"] += carried
        res["usd"] += usd
        k = m.get("kind", "?")
        b = res["by"].setdefault(k, [0, 0, 0])
        b[0] += tok
        b[1] += carried
        b[2] += 1
    return res


# ---------------------------------------------------------------- formatting
def H(n):
    n = float(n)
    for unit, d in (("B", 1e9), ("M", 1e6), ("k", 1e3)):
        if abs(n) >= d:
            return ("%.1f%s" % (n / d, unit)).replace(".0" + unit, unit)
    return "%d" % n


def billing():
    b = util.cfg().get("billing")
    if b in ("api", "subscription"):
        return b
    return "api" if os.environ.get("ANTHROPIC_API_KEY") else "subscription"


# ---------------------------------------------------------------- status line
def statusline():
    try:
        ev = json.load(sys.stdin)
    except Exception:
        ev = {}
    sid, tp = ev.get("session_id"), ev.get("transcript_path")
    reqs, comps = [], []
    for f in session_files(sid, tp):
        st = parse_transcript(f)
        reqs += list(st["req"].values())
        comps += st.get("compacts", [])
    reqs.sort(key=lambda r: r[0])
    s = summarize(reqs)
    if not reqs:
        cw = ev.get("context_window") or {}
        s["total_in"] = cw.get("total_input_tokens") or 0
        s["out"] = cw.get("total_output_tokens") or 0
        s["cost"] = (ev.get("cost") or {}).get("total_cost_usd") or 0
    sv = savings(load_metrics(sid), reqs, comps)
    rl = ev.get("rate_limits") or {}
    if rl:
        try:
            with open(util.path("rate.json"), "w") as f:
                json.dump({"ts": time.time(), "rate_limits": rl, "sid": sid}, f)
        except Exception:
            pass
    ctx = (ev.get("context_window") or {}).get("used_percentage")
    cached = (s["cr"] / s["total_in"] * 100) if s["total_in"] else 0
    pct = sv["usd"] / (s["cost"] + sv["usd"]) * 100 if s["cost"] + sv["usd"] > 0 else 0
    left = []
    if billing() == "subscription" and rl:
        for key, lab in (("five_hour", "5h"), ("seven_day", "7d")):
            w = rl.get(key) or {}
            u = w.get("used_percentage")
            if u is not None:
                wo = u * (1 + pct / (100 - pct)) if pct < 100 else u
                left.append("%s %d%%%s" % (lab, round(u), " (≈%d%% w/o lean)" % round(wo) if sv["usd"] > 0 and lab == "5h" else ""))
        left.append("%s tok" % H(s["total_in"] + s["out"]))
    else:
        left.append("%s in (%d%% cached) · %s out · $%.2f" % (H(s["total_in"]), round(cached), H(s["out"]), s["cost"]))
    if ctx is not None:
        left.append("ctx %d%%" % round(ctx))
    right = "lean saved %s tok · $%.2f (%d%%)" % (H(sv["carried"]), sv["usd"], round(pct)) if sv["direct"] else "lean: no savings yet"
    if billing() == "subscription" and sv["usd"] > 0 and pct < 100:
        right += " · ×%.2f capacity" % (100 / (100 - pct))
    print("⛁ " + " · ".join(left) + " │ " + right)
    return 0


# ---------------------------------------------------------------- x stats
def _range(scope):
    now = time.time()
    if scope == "today":
        d = dt.datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)
        return d.timestamp(), "today"
    if scope == "week":
        return now - 7 * 86400, "last 7 days"
    if scope == "5h":
        return now - 5 * 3600, "last 5 hours"
    if scope == "all":
        return 0, "all time"
    return None, "this session"


def _all_transcripts(since):
    fs = glob.glob(os.path.join(projects_dir(), "*", "*.jsonl")) + glob.glob(os.path.join(projects_dir(), "*", "*", "subagents", "*.jsonl"))
    return [f for f in fs if os.path.getmtime(f) >= since]


def stats(args):
    scope = args[0] if args else "session"
    since, label = _range(scope)
    per_session = {}
    if since is None:
        sid = os.environ.get("LEAN_SID")
        files = session_files(sid) if sid else []
        if not files:
            fs = _all_transcripts(0)
            fs = [f for f in fs if "/subagents/" not in f]
            if fs:
                latest = max(fs, key=os.path.getmtime)
                sid = os.path.basename(latest)[:-6]
                files = session_files(transcript=latest)
        since = 0
        for f in files:
            st = parse_transcript(f)
            per_session.setdefault(sid, ([], []))
            per_session[sid][0].extend(st["req"].values())
            per_session[sid][1].extend(st.get("compacts", []))
        metrics = load_metrics(sid)
    else:
        for f in _all_transcripts(since):
            st = parse_transcript(f)
            sid = os.path.basename(f)[:-6] if "/subagents/" not in f else f.split(os.sep)[-3]
            rows = [r for r in st["req"].values() if r[0] >= since]
            e = per_session.setdefault(sid, ([], []))
            e[0].extend(rows)
            e[1].extend(st.get("compacts", []))
        metrics = load_metrics(None, since)
    allrows = [r for v in per_session.values() for r in v[0]]
    s = summarize(allrows)
    by_sid = {}
    for m in metrics:
        by_sid.setdefault(m.get("sid"), []).append(m)
    tot = {"direct": 0, "carried": 0, "usd": 0.0, "by": {}, "missed": 0, "missed_n": 0}
    for sid, ms in by_sid.items():
        rows, comps = per_session.get(sid, ([], []))
        sv = savings(ms, sorted(rows, key=lambda r: r[0]), comps)
        for k in ("direct", "carried", "usd", "missed", "missed_n"):
            tot[k] += sv[k]
        for k, v in sv["by"].items():
            b = tot["by"].setdefault(k, [0, 0, 0])
            for i in range(3):
                b[i] += v[i]
    pct = tot["usd"] / (s["cost"] + tot["usd"]) * 100 if s["cost"] + tot["usd"] else 0
    out = ["lean stats — %s · %d sessions · %d requests" % (label, len([k for k, v in per_session.items() if v[0]]), s["n"])]
    out.append("usage   in %s (cache read %s · write %s · fresh %s) · out %s · ≈$%.2f API-equivalent" % (
        H(s["total_in"]), H(s["cr"]), H(s["cw"]), H(s["in"]), H(s["out"]), s["cost"]))
    if s["models"]:
        out.append("models  " + ", ".join("%s $%.2f" % (k or "?", v) for k, v in sorted(s["models"].items(), key=lambda kv: -kv[1])))
    out.append("saved   direct %s tok · carried %s tok · ≈$%.2f → %.0f%% of what this would have cost" % (H(tot["direct"]), H(tot["carried"]), tot["usd"], pct))
    if pct > 0:
        out.append("        same budget/limits stretch ×%.2f (subscription 5h/7d windows scale the same way, ≈)" % (100 / (100 - pct)))
    if tot["missed_n"]:
        out.append("missed  %d failed calls printed raw (Claude Code can't rewrite failures): ~%s tok avoidable with t / c / x run" % (tot["missed_n"], H(tot["missed"])))
    if tot["by"]:
        out.append("by      " + " · ".join("%s %s (%d×)" % (k, H(v[0]), v[2]) for k, v in sorted(tot["by"].items(), key=lambda kv: -kv[1][0])))
    try:
        with open(util.path("rate.json")) as f:
            rj = json.load(f)
        rl = rj.get("rate_limits", {})
        bits = []
        for key, lab in (("five_hour", "5h"), ("seven_day", "7d")):
            w = rl.get(key) or {}
            if w.get("used_percentage") is not None:
                r = w.get("resets_at")
                bits.append("%s %d%% used%s" % (lab, round(w["used_percentage"]), (", resets %s" % dt.datetime.fromtimestamp(r).strftime("%a %H:%M")) if r else ""))
        if bits:
            out.append("plan    " + " · ".join(bits) + "  (as of %s)" % dt.datetime.fromtimestamp(rj["ts"]).strftime("%H:%M"))
    except Exception:
        pass
    out.append("tokens≈chars/%.2f%s" % (util.chars_per_token(), "" if util.cfg().get("chars_per_token") else " (default; run `x calibrate`)"))
    print("\n".join(out))
    return 0


# ---------------------------------------------------------------- x waste
def waste(args):
    n = int(args[0]) if args and args[0].isdigit() else 15
    since = time.time() - 7 * 86400
    groups = {}
    for f in _all_transcripts(since):
        uses = {}
        try:
            fh = open(f, errors="replace")
        except Exception:
            continue
        for l in fh:
            try:
                d = json.loads(l)
            except Exception:
                continue
            m = d.get("message") or {}
            c = m.get("content")
            if not isinstance(c, list):
                continue
            for b in c:
                if not isinstance(b, dict):
                    continue
                if b.get("type") == "tool_use":
                    inp = b.get("input") or {}
                    name = b.get("name", "?")
                    if name == "Bash":
                        w = (inp.get("command") or "").strip().split("\n")[0]
                        from .filt import main_segment
                        key = " ".join(main_segment(w).split()[:2]) or w[:30]
                    elif name in ("Read", "Edit", "Write"):
                        key = os.path.splitext(inp.get("file_path", ""))[1] or "?"
                    else:
                        key = ""
                    uses[b.get("id")] = (name, key)
                elif b.get("type") == "tool_result":
                    cont = b.get("content")
                    size = len(cont) if isinstance(cont, str) else sum(len(x.get("text", "")) for x in cont or [] if isinstance(x, dict))
                    name, key = uses.get(b.get("tool_use_id"), ("?", ""))
                    g = groups.setdefault((name, key), [0, 0, 0])
                    g[0] += size
                    g[1] += 1
                    g[2] = max(g[2], size)
    rows = sorted(groups.items(), key=lambda kv: -kv[1][0])[:n]
    print("biggest tool outputs entering context (last 7 days) — tokens≈chars/%.1f" % util.chars_per_token())
    print("%-8s %-6s %-8s  %s" % ("tokens", "calls", "max", "tool · command/ext"))
    for (name, key), (tot, cnt, mx) in rows:
        print("%-8s %-6d %-8s  %s %s" % (H(util.tokens(tot)), cnt, H(util.tokens(mx)), name, key))
    return 0


# ---------------------------------------------------------------- x calibrate
def calibrate():
    """Fit chars/token: for request pairs (i, i+1) separated by exactly one large tool result,
    Δ(total input) − output_i ≈ tokens of that result."""
    ratios = []
    for f in _all_transcripts(time.time() - 30 * 86400)[:200]:
        seq = []   # ordered: ('req', total_in, out) | ('res', chars)
        seen = set()
        try:
            for l in open(f, errors="replace"):
                d = json.loads(l)
                m = d.get("message") or {}
                if d.get("type") == "assistant" and isinstance(m.get("usage"), dict):
                    if m.get("id") in seen:
                        continue
                    seen.add(m.get("id"))
                    u = m["usage"]
                    tot = (u.get("input_tokens") or 0) + (u.get("cache_creation_input_tokens") or 0) + (u.get("cache_read_input_tokens") or 0)
                    seq.append(("req", tot, u.get("output_tokens") or 0))
                elif d.get("type") == "user" and isinstance(m.get("content"), list):
                    for b in m["content"]:
                        if isinstance(b, dict) and b.get("type") == "tool_result":
                            c = b.get("content")
                            size = len(c) if isinstance(c, str) else sum(len(x.get("text", "")) for x in c or [] if isinstance(x, dict))
                            seq.append(("res", size))
        except Exception:
            continue
        for i in range(len(seq) - 2):
            a, b, c = seq[i], seq[i + 1], seq[i + 2]
            if a[0] == "req" and b[0] == "res" and c[0] == "req" and b[1] >= 4000:
                tok = c[1] - a[1] - a[2]
                if tok > 500:
                    r = b[1] / tok
                    if 1.5 < r < 8:
                        ratios.append(r)
    if len(ratios) < 5:
        print("calibrate: only %d usable samples (need ≥5); keeping %.2f chars/token" % (len(ratios), util.chars_per_token()))
        return 1
    ratios.sort()
    med = ratios[len(ratios) // 2]
    c = util.cfg()
    c["chars_per_token"] = round(med, 3)
    util.save_cfg(c)
    print("calibrate: %d samples · median %.2f chars/token (p25 %.2f, p75 %.2f) → saved" % (len(ratios), med, ratios[len(ratios) // 4], ratios[3 * len(ratios) // 4]))
    return 0
