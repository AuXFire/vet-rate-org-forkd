#!/usr/bin/env bash
# lean installer (idempotent).
#   ./install.sh               link tools into ~/.local/bin, register hooks + status line in ~/.claude/settings.json
#   ./install.sh --no-status   same, but leave your status line alone
#   ./install.sh --uninstall   remove links, hooks and status line entries
#   Plugin route instead:      claude plugin marketplace add <owner>/<repo> && claude plugin install lean@lean
set -euo pipefail
ROOT="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${LEAN_BIN:-$HOME/.local/bin}"
SETTINGS="${CLAUDE_SETTINGS:-$HOME/.claude/settings.json}"
MODE="${1:-install}"
command -v python3 >/dev/null || { echo "lean: python3 is required" >&2; exit 1; }

mkdir -p "$BIN" "$(dirname "$SETTINGS")"
for n in lean t c o g sub x; do
  if [ "$MODE" = "--uninstall" ]; then
    [ -L "$BIN/$n" ] && [ "$(readlink "$BIN/$n")" = "$ROOT/bin/lean" ] && rm -f "$BIN/$n"
  else
    if [ -e "$BIN/$n" ] && [ ! -L "$BIN/$n" ]; then echo "lean: skip $BIN/$n (exists, not ours)"; continue; fi
    ln -sfn "$ROOT/bin/lean" "$BIN/$n"
  fi
done

python3 - "$SETTINGS" "$ROOT" "$MODE" <<'PY'
import json, os, shutil, sys
path, root, mode = sys.argv[1], sys.argv[2], sys.argv[3]
cmd = '"%s/bin/lean" hook' % root
try:
    s = json.load(open(path))
except FileNotFoundError:
    s = {}
except Exception as e:
    sys.exit("lean: %s is not valid JSON (%s); fix it and re-run" % (path, e))
if os.path.exists(path):
    shutil.copy(path, path + ".lean-backup")
hooks = s.setdefault("hooks", {})
# drop any previous lean entries
for ev in list(hooks):
    groups = []
    for g in hooks[ev]:
        g["hooks"] = [h for h in g.get("hooks", []) if "/bin/lean\" hook" not in h.get("command", "") and "/bin/lean hook" not in h.get("command", "")]
        if g["hooks"]:
            groups.append(g)
    if groups:
        hooks[ev] = groups
    else:
        del hooks[ev]
st = s.get("statusLine", {})
ours = "bin/lean" in st.get("command", "")
if mode == "--uninstall":
    if ours:
        s.pop("statusLine")
    if not hooks:
        s.pop("hooks", None)
else:
    plugin = json.load(open(os.path.join(root, "hooks", "hooks.json")))["hooks"]
    for ev, groups in plugin.items():
        for g in groups:
            g = json.loads(json.dumps(g).replace("${CLAUDE_PLUGIN_ROOT}", root))
            hooks.setdefault(ev, []).append(g)
    if mode != "--no-status":
        if st and not ours:
            print("lean: keeping your existing statusLine; to use lean's: set statusLine.command to '%s/bin/lean statusline'" % root)
        else:
            s["statusLine"] = {"type": "command", "command": '"%s/bin/lean" statusline' % root, "padding": 0}
json.dump(s, open(path, "w"), indent=2)
print("lean: %s %s" % ("removed from" if mode == "--uninstall" else "registered in", path))
PY

if [ "$MODE" != "--uninstall" ]; then
  case ":$PATH:" in *":$BIN:"*) ;; *) echo "lean: note: $BIN is not on PATH in this shell (Claude Code sessions get it via the SessionStart hook)";; esac
  echo "lean: installed. Tools: t c o g sub x · try: x stats · restart Claude Code (or /hooks) to load hooks."
fi
