"""Claude Code hook entry point (one process per event; must be fast and never fail loudly).

PostToolUse  Bash          -> filtered view via updatedToolOutput (raw kept in a log)
PostToolUse  mcp__*        -> lossless JSON minify via updatedMCPToolOutput
PostToolUse  Edit|Write    -> fast per-file syntax/lint check; record edited file
PreToolUse   Read          -> large-file guard: deny once with an outline, allow on retry
SessionStart               -> env file (quiet tools) + project brief; + task state on resume/compact
SubagentStart              -> short brief
PreCompact                 -> stdout steers the summary to keep task state
"""
import ast
import json
import os
import re
import shutil
import sys
import time
from . import filt, util

CODE_GUARD_EXT = {".py", ".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs", ".go", ".rs", ".java", ".kt", ".cs", ".rb", ".php",
                  ".c", ".h", ".cc", ".cpp", ".hpp", ".swift", ".scala", ".vue", ".svelte", ".md", ".mdx"}
ENV_VARS = {k: v for k, v in util.QUIET_ENV.items() if k not in ("CI", "TERM")}
OWN = ("t", "c", "o", "g", "sub", "x", "lean")
TOOLS_LINE = ("tools: t=test(smart: reruns failures, related tests) · c=typecheck+lint · o <file|dir|def X|refs X|pkg.attr>=outline/defs · "
              "g s|d|l=git · sub=find/replace · `x run <cmd>` for any other noisy command that may fail (failed calls are not auto-filtered) · RAW=1 skips filtering")


def _sess(sid):
    return util.path("sess", (sid or "nosid") + ".json")


def load_sess(sid):
    try:
        with open(_sess(sid)) as f:
            return json.load(f)
    except Exception:
        return {}


def save_sess(sid, s):
    try:
        with open(_sess(sid), "w") as f:
            json.dump(s, f)
    except Exception:
        pass


def emit(obj):
    sys.stdout.write(json.dumps(obj))
    sys.stdout.flush()


# ---------------------------------------------------------------- brief / state
def brief(cwd, full=True):
    from . import detect
    r = util.root(cwd)
    info = detect.detect(cwd)
    lines = []
    c, br = util.sh(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=r, timeout=5)
    if c == 0:
        _, st = util.sh(["git", "status", "--porcelain"], cwd=r, timeout=10)
        _, last = util.sh(["git", "log", "-1", "--format=%s (%ar)"], cwd=r, timeout=5)
        n = len([l for l in st.split("\n") if l.strip()])
        lines.append("repo %s · branch %s · %s · last: %s" % (os.path.basename(r), br.strip(), "%d changed files" % n if n else "clean", last.strip()[:80]))
    else:
        lines.append("dir %s (not a git repo)" % util.short(r))
    if info["stacks"]:
        rel = os.path.relpath(info["dir"], r)
        lines.append("stack: %s%s · test: %s · check: %s" % (
            ",".join(info["stacks"]), "" if rel == "." else " (in %s)" % rel,
            ", ".join(t["runner"] for t in info["tests"]) or "-", ", ".join(c["name"] for c in info["checks"]) or "-"))
    if full:
        c, out = util.sh(["git", "ls-files"], cwd=r, timeout=10)
        if c == 0 and out.strip():
            cnt = {}
            for f in out.split("\n"):
                if f:
                    top = f.split("/")[0] + ("/" if "/" in f else "")
                    cnt[top] = cnt.get(top, 0) + 1
            dirs = sorted(((k, v) for k, v in cnt.items() if k.endswith("/")), key=lambda kv: -kv[1])[:8]
            files = len([k for k in cnt if not k.endswith("/")])
            lines.append("top: " + ", ".join("%s%d" % (k, v) for k, v in dirs) + (" · %d root files" % files if files else ""))
        lines.append(TOOLS_LINE)
    return "[lean] " + "\n".join(lines)


def task_state(cwd, sid):
    r = util.root(cwd)
    st = util.load_state(r)
    ss = load_sess(sid)
    out = []
    ed = ss.get("edited", [])
    if ed:
        out.append("edited this session: " + ", ".join(os.path.relpath(f, r) for f in ed[-15:]))
    lt = st.get("last_test")
    if lt:
        out.append("last test: %s%s" % (lt.get("summary", ""), (" · failing: " + ", ".join(lt["failed"][:8])) if lt.get("failed") else ""))
    lc = st.get("last_check")
    if lc:
        out.append("last check: " + lc.get("summary", ""))
    return "\n".join(out)


# ---------------------------------------------------------------- handlers
def on_session_start(ev):
    sid, cwd = ev.get("session_id"), ev.get("cwd") or os.getcwd()
    envf = os.environ.get("CLAUDE_ENV_FILE")
    if envf and util.cfg().get("quiet_env", True):
        try:
            with open(envf, "a") as f:
                for k, v in ENV_VARS.items():
                    f.write("export %s=%s\n" % (k, v))
                f.write("export LEAN_SID=%s\n" % sid)
                b = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "bin")
                if b not in os.environ.get("PATH", "").split(":"):
                    f.write('export PATH="%s:$PATH"\n' % b)
        except Exception:
            pass
    ctx = brief(cwd)
    if ev.get("source") in ("resume", "compact"):
        ts = task_state(cwd, sid)
        if ts:
            ctx += "\n[lean task state]\n" + ts
    emit({"hookSpecificOutput": {"hookEventName": "SessionStart", "additionalContext": ctx}})


def on_subagent_start(ev):
    emit({"hookSpecificOutput": {"hookEventName": "SubagentStart", "additionalContext": brief(ev.get("cwd") or os.getcwd(), full=False) + "\n" + TOOLS_LINE}})


def on_pre_compact(ev):
    ts = task_state(ev.get("cwd") or os.getcwd(), ev.get("session_id"))
    if ts:
        print("Keep this task state verbatim in the summary:\n" + ts[:2000])


def on_pre_read(ev):
    ti, sid = ev.get("tool_input") or {}, ev.get("session_id")
    p = ti.get("file_path") or ""
    if not p or not os.path.isfile(p):
        return
    ss = load_sess(sid)
    guarded = ss.setdefault("guarded", {})
    if ti.get("offset") or ti.get("limit") or ti.get("pages"):
        if p in guarded:     # attribute the follow-up partial read as cost, for honest accounting
            try:
                with open(p, errors="replace") as fh:
                    lines = fh.read().split("\n")
                a = max(0, int(ti.get("offset") or 1) - 1)
                seg = lines[a:a + int(ti.get("limit") or 2000)]
                util.metric("read-followup", 0, sum(len(l) + 8 for l in seg), sid=sid, file=os.path.basename(p))
            except Exception:
                pass
        return
    ext = os.path.splitext(p)[1].lower()
    limit = int(util.cfg().get("read_guard_lines", 400))
    if ext not in CODE_GUARD_EXT or limit <= 0:
        return
    if re.search(r"/(skills|plugins|bundled-skills|\.claude)/|(SKILL|CLAUDE|AGENTS)\.md$", p):
        return          # instructions the agent was told to read in full
    if ext in (".md", ".mdx"):
        limit *= 2
    try:
        with open(p, errors="replace") as fh:
            text = fh.read()
    except Exception:
        return
    n = text.count("\n") + 1
    if n <= limit:
        return
    if p in guarded:   # second full read: allow, and book the cost back
        util.metric("read-guard-retry", 0, len(text), sid=sid, file=os.path.basename(p))
        guarded.pop(p, None)
        save_sess(sid, ss)
        return
    from . import nav
    try:
        syms = nav.outline(p)
        ol = nav.fmt_outline(p, syms, n, max_depth=2) if syms else ""
    except Exception:
        ol = ""
    if not ol or len(ol) > len(text) * 0.5:
        return
    guarded[p] = time.time()
    save_sess(sid, ss)
    reason = ("lean read-guard: %s has %d lines. Outline below — Read with offset/limit for the part you need "
              "(or `o %s <symbol>`). Reading the same file again without offset returns all of it.\n%s") % (p, n, p, ol)
    util.metric("read-guard", len(text), len(reason), sid=sid, file=os.path.basename(p))
    emit({"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny", "permissionDecisionReason": reason}})


def on_post_bash(ev):
    ti, tr = ev.get("tool_input") or {}, ev.get("tool_response")
    if not isinstance(tr, dict) or ti.get("run_in_background"):
        return
    cmd = ti.get("command") or ""
    out, err = tr.get("stdout") or "", tr.get("stderr") or ""
    if not isinstance(out, str) or not isinstance(err, str) or tr.get("isImage"):
        return
    raw = out + ("\n" + err if err.strip() else "")
    if len(raw) < 600:
        return
    m = filt.main_segment(cmd).split()
    if m and os.path.basename(m[0].strip("'\"")) in OWN:
        return          # already compact
    code = tr.get("exitCode", tr.get("returnCode", tr.get("code")))
    code = code if isinstance(code, int) else None
    shown, kind = filt.filter_output(cmd, raw, ev.get("cwd"), code=code)
    if kind in ("raw", "read"):
        return
    log = util.save_log(raw, (m[0] if m else "bash"))
    footer = "\n[lean %d→%d lines · raw: %s]" % (raw.count("\n") + 1, shown.count("\n") + 1, log)
    if len(shown) + len(footer) >= len(raw) * 0.85:
        return
    shown = shown.rstrip("\n") + footer
    util.metric("bash:" + kind, len(raw), len(shown), sid=ev.get("session_id"), cmd=cmd[:120])
    new = dict(tr)
    new["stdout"], new["stderr"] = shown, ""
    emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "updatedToolOutput": new}})


def rewrite_enabled(ev):
    """Rewriting a command is only safe where no permission rule or classifier reviews the input."""
    mode = util.cfg().get("rewrite", "auto")
    if mode in (False, "off", "never"):
        return False
    if mode in (True, "on", "always"):
        return True
    return ev.get("permission_mode") == "bypassPermissions"


def on_pre_bash(ev):
    ti = ev.get("tool_input") or {}
    cmd = ti.get("command") or ""
    if not cmd or ti.get("run_in_background") or not rewrite_enabled(ev):
        return
    m = filt.main_segment(cmd).split()
    if not m or os.path.basename(m[0].strip("'\"")) in OWN or filt.kind_of(cmd) not in ("test", "build", "install"):
        return
    import shlex
    lean = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "bin", "lean")
    new = dict(ti, command="%s run %s" % (shlex.quote(lean), shlex.quote(cmd)))
    emit({"hookSpecificOutput": {"hookEventName": "PreToolUse", "updatedInput": new}})


def on_post_failure(ev):
    """Failed calls can't be rewritten (Claude Code passes them through). Record the miss; tip once per session."""
    err = ev.get("error")
    if not isinstance(err, str) or len(err) < 4000 or ev.get("tool_name") != "Bash":
        return
    cmd = (ev.get("tool_input") or {}).get("command") or ""
    m = filt.main_segment(cmd).split()
    if m and os.path.basename(m[0].strip("'\"")) in OWN:
        return
    shown, kind = filt.filter_output(cmd, err, ev.get("cwd"))
    sid = ev.get("session_id")
    util.metric("missed:" + kind, len(err), len(err), sid=sid, cmd=cmd[:120], could=len(shown))
    ss = load_sess(sid)
    if not ss.get("tipped") and len(shown) < len(err) * 0.6:
        ss["tipped"] = True
        save_sess(sid, ss)
        tool = "t" if kind == "test" else "c" if kind == "build" else "x run <cmd>"
        emit({"hookSpecificOutput": {"hookEventName": "PostToolUseFailure", "additionalContext":
              "lean: that failure printed ~%d tokens; `%s` would have shown ~%d with the same facts." % (util.tokens(len(err)), tool, util.tokens(len(shown)))}})


def on_post_search(ev):
    """Built-in Glob / Grep: lossless, order-preserving compaction (common prefix once, grouped by file)."""
    tr, tool = ev.get("tool_response"), ev.get("tool_name")
    if not isinstance(tr, dict):
        return
    new = dict(tr)
    if isinstance(tr.get("content"), str) and tr.get("content"):
        raw = tr["content"]
        out = filt.compact_grep(raw)
        if not out or len(out) > len(raw) * 0.85:
            return
        new["content"] = out
    elif isinstance(tr.get("filenames"), list) and all(isinstance(x, str) for x in tr["filenames"]):
        raw = "\n".join(tr["filenames"])
        out = filt.compact_paths(tr["filenames"])
        if not out or len(out) > len(raw) * 0.85:
            return
        new["filenames"] = [out]
    else:
        return
    util.metric(tool.lower(), len(raw), len(out), sid=ev.get("session_id"), cmd=json.dumps(ev.get("tool_input") or {})[:120])
    emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "updatedToolOutput": new}})


def on_post_mcp(ev):
    """Any MCP server (current or added later): compact JSON text results; raw kept in a log."""
    from . import jsonc
    tr, sid, tool = ev.get("tool_response"), ev.get("session_id"), ev.get("tool_name")
    blocks = tr if isinstance(tr, list) else [{"type": "text", "text": tr}] if isinstance(tr, str) else None
    if not blocks or not all(isinstance(x, dict) for x in blocks):
        return
    new, raw_len, info_all = [], 0, {"pruned": 0, "tables": 0}
    for x in blocks:
        t = x.get("text") if x.get("type") == "text" else None
        if isinstance(t, str):
            raw_len += len(t)
            out, info = jsonc.compact(t)
            if out is not None:
                x = dict(x, text=out)
                for k in info_all:
                    info_all[k] += info[k]
        new.append(x)
    shown = sum(len(x.get("text", "")) for x in new if isinstance(x.get("text"), str))
    if shown >= raw_len * 0.92:
        return
    log = util.save_log("\n".join(x.get("text", "") for x in blocks if isinstance(x.get("text"), str)), tool.split("__")[-1])
    note = jsonc.note(info_all, log)
    util.metric("mcp:" + tool.split("__")[1] if tool.count("__") >= 2 else "mcp", raw_len, shown + len(note), sid=sid, cmd=tool)
    if isinstance(tr, str):
        emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "updatedMCPToolOutput": new[0]["text"] + "\n" + note}})
    else:
        emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "updatedMCPToolOutput": new + [{"type": "text", "text": note}]}})


def quick_check(p):
    """Fast (<~2s) syntax/correctness check for one file. Returns error text or ''."""
    ext = os.path.splitext(p)[1].lower()
    try:
        if ext == ".py":
            src = open(p, errors="replace").read()
            try:
                ast.parse(src, p)
            except SyntaxError as e:
                return "%s:%s:%s: SyntaxError: %s" % (os.path.basename(p), e.lineno, e.offset, e.msg)
            if shutil.which("ruff"):
                c, out = util.sh(["ruff", "check", "--no-cache", "--output-format", "concise", "--select", "E9,F63,F7,F82,F811", p], timeout=5)
                if c not in (0, 2):
                    return "\n".join(l for l in out.split("\n") if re.match(r"^\S+:\d+:\d+:", l))[:1500]
            return ""
        if ext == ".json":
            try:
                json.loads(open(p).read())
            except ValueError as e:
                return "%s: invalid JSON: %s" % (os.path.basename(p), e)
            return ""
        if ext == ".toml":
            try:
                import tomllib
                tomllib.loads(open(p).read())
            except ImportError:
                return ""
            except Exception as e:
                return "%s: invalid TOML: %s" % (os.path.basename(p), e)
            return ""
        if ext in (".js", ".mjs", ".cjs") and shutil.which("node"):
            c, out = util.sh(["node", "--check", p], timeout=5)
            return _first_err(out) if c else ""
        if ext in (".ts", ".mts", ".cts", ".tsx", ".jsx"):
            esb = shutil.which("esbuild")
            if esb:
                c, out = util.sh([esb, p, "--log-level=error", "--outfile=/dev/null", "--loader:%s=%s" % (ext, ext[1:].replace("m", "").replace("c", ""))], timeout=5)
                return _first_err(out) if c else ""
            if ext in (".ts", ".mts", ".cts") and shutil.which("node"):
                c, out = util.sh(["node", "--experimental-strip-types", "--no-warnings", "--check", p], timeout=5)
                if c and "ERR_UNSUPPORTED" not in out and "bad option" not in out:
                    return _first_err(out)
            return ""
        if ext == ".go" and shutil.which("gofmt"):
            c, out = util.sh(["gofmt", "-e", "-l", p], timeout=5)
            return out.strip()[:1500] if c else ""
        if ext == ".rs" and shutil.which("rustfmt"):
            c, out = util.sh("rustfmt --edition 2021 --emit stdout %s >/dev/null" % json.dumps(p), timeout=5)
            return "\n".join(l for l in out.split("\n") if l.strip())[:1500] if c else ""
    except Exception:
        return ""
    return ""


def _first_err(out):
    ls = [l for l in out.split("\n") if l.strip() and not l.startswith("Node.js v") and not l.strip().startswith("at ")]
    return "\n".join(ls[:6])[:1200]


def on_post_edit(ev):
    ti, sid = ev.get("tool_input") or {}, ev.get("session_id")
    p = ti.get("file_path") or ti.get("notebook_path")
    if not p:
        return
    ss = load_sess(sid)
    ed = [x for x in ss.get("edited", []) if x != p] + [p]
    ss["edited"] = ed[-50:]
    save_sess(sid, ss)
    if not util.cfg().get("edit_check", True) or not os.path.isfile(p):
        return
    err = quick_check(p)
    if err:
        emit({"hookSpecificOutput": {"hookEventName": "PostToolUse",
                                     "additionalContext": "lean edit-check ✗ (the edit was applied; fix this next):\n" + err}})


def main():
    try:
        ev = json.load(sys.stdin)
    except Exception:
        return 0
    debug = os.environ.get("LEAN_DEBUG") or util.cfg().get("debug")
    if debug:
        try:
            with open(util.path("debug", "events.jsonl"), "a") as f:
                f.write(json.dumps(ev)[:20000] + "\n")
        except Exception:
            pass
    name, tool = ev.get("hook_event_name"), ev.get("tool_name") or ""
    try:
        if name == "PostToolUse":
            if tool == "Bash":
                on_post_bash(ev)
            elif tool in ("Edit", "Write", "MultiEdit", "NotebookEdit"):
                on_post_edit(ev)
            elif tool in ("Glob", "Grep"):
                on_post_search(ev)
            elif tool.startswith("mcp__"):
                on_post_mcp(ev)
        elif name == "PreToolUse" and tool == "Read":
            on_pre_read(ev)
        elif name == "PreToolUse" and tool == "Bash":
            on_pre_bash(ev)
        elif name == "PostToolUseFailure":
            on_post_failure(ev)
        elif name == "SessionStart":
            on_session_start(ev)
        elif name == "SubagentStart":
            on_subagent_start(ev)
        elif name == "PreCompact":
            on_pre_compact(ev)
    except Exception:
        if debug:
            import traceback
            with open(util.path("debug", "errors.log"), "a") as f:
                f.write(traceback.format_exc())
    return 0
