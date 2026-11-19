"""JSON compaction for tool output (MCP results, `gh api`, `curl`, …).

Lossless steps (always):
  - minify, write non-ASCII and escaped characters (\\u003c) as themselves
  - tabularize lists of same-shaped objects:  [{"a":1,"b":2},…] -> {"_cols":["a","b"],"_rows":[[1,2],…]}
Recoverable step (only if config json_prune is on; the raw output is logged):
  - drop link-noise keys: *_url (except html_url / url / download_url), node_id, gravatar_id, _links
"""
import json
import re
from . import util

KEEP_URL = {"html_url", "url", "download_url", "web_url", "permalink_url"}
NOISE = re.compile(r"^(node_id|gravatar_id|_links|.+_url)$")
MIN_ROWS = 3


def _prune(o, n):
    if isinstance(o, dict):
        out = {}
        for k, v in o.items():
            if isinstance(k, str) and NOISE.match(k) and k not in KEEP_URL:
                n[0] += 1
                continue
            out[k] = _prune(v, n)
        return out
    if isinstance(o, list):
        return [_prune(x, n) for x in o]
    return o


def _table(o, n):
    if isinstance(o, dict):
        return {k: _table(v, n) for k, v in o.items()}
    if isinstance(o, list):
        items = [_table(x, n) for x in o]
        if len(items) >= MIN_ROWS and all(isinstance(x, dict) for x in items):
            keys = list(items[0].keys())
            if len(keys) >= 2 and all(list(x.keys()) == keys for x in items):
                n[0] += 1
                same = {k: items[0][k] for k in keys if all(x[k] == items[0][k] for x in items)} if len(items) >= 5 else {}
                cols = [k for k in keys if k not in same]
                t = {"_cols": cols, "_rows": [[x[k] for k in cols] for x in items]}
                if same:
                    t["_same"] = same          # columns with one value in every row, stated once
                return t
        return items
    return o


def compact(text, prune=None, min_len=800):
    """-> (compacted_text, info) or (None, None) when not JSON / not worth it."""
    s = text.strip()
    if len(s) < min_len or s[:1] not in "[{":
        return None, None
    try:
        obj = json.loads(s)
    except Exception:
        return None, None
    if prune is None:
        prune = util.cfg().get("json_prune", True)
    pruned, tables = [0], [0]
    if prune:
        obj = _prune(obj, pruned)
    obj = _table(obj, tables)
    out = json.dumps(obj, separators=(",", ":"), ensure_ascii=False)
    if len(out) >= len(s) * 0.92:
        return None, None
    return out, {"pruned": pruned[0], "tables": tables[0]}


def note(info, log=None):
    bits = []
    if info.get("tables"):
        bits.append("lists of same-shaped objects as _cols/_rows (+_same: value shared by every row)")
    if info.get("pruned"):
        bits.append("dropped %d *_url/node_id fields" % info["pruned"])
    return "[lean: json compacted%s%s]" % ((": " + "; ".join(bits)) if bits else "", (" · raw: %s" % log) if log else "")


def expand(o):
    """Inverse of tabularization (used by tests; also documents the format)."""
    if isinstance(o, dict):
        if set(o) in ({"_cols", "_rows"}, {"_cols", "_rows", "_same"}):
            rows = [dict(zip(o["_cols"], [expand(v) for v in r])) for r in o["_rows"]]
            for r in rows:
                r.update(expand(o.get("_same", {})))
            return rows
        return {k: expand(v) for k, v in o.items()}
    if isinstance(o, list):
        return [expand(x) for x in o]
    return o
