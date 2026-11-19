# lean

by [AuXFire](https://github.com/AuXFire)

Token-efficient tooling for Claude Code and other coding agents. It cuts what tool output costs in context without losing information, so more of the model's budget goes to the actual task.

Most of the token cost in agent work isn't the model's writing. It's tool output (test logs, build errors, diffs, whole-file reads) that enters the context and is re-sent with every later request. lean compresses that output at the source, and every compression can be recovered.

## Rules

- **Passing runs take one line; failures show everything that matters.** That means failing test names, assertion diffs, `file:line` and a 3-line code snippet. Identical failures are grouped (`×40 TypeError … @ auth.ts:88`).
- **Nothing is unrecoverable.** The full raw output always goes to a log. Filtered results print the log path, and `x last` shows the most recent one. Prefix any command with `RAW=1` to skip filtering.
- **Exit codes are preserved, and anything unexpected falls back to raw output.** If the filter can't explain a failure, the generic squeeze still keeps every error line.
- **Source code is never compressed.** `cat`/`head`/`sed`, pipes and reads pass through untouched, because edits need exact text.
- **No dependencies.** It needs Python 3.8+ and bash. `rg`, `ruff`, `esbuild`, `gofmt` and `rustfmt` are used when present.

## What it does

| Part | How it saves tokens |
|---|---|
| **Output filter** (PostToolUse hook) | Test, build, lint, install, git and docker output from **successful** calls is replaced with a compact view. Unknown commands get a near-lossless squeeze: ANSI and progress redraws removed, repeats collapsed, very long lines capped, head/tail kept along with every error line. Lockfile and generated-file hunks in diffs collapse to `+N −M`. |
| **MCP and JSON compaction** | Every MCP server is covered automatically, including ones added later, through the `mcp__.*` hook. The same applies to JSON printed by shell commands (`gh api`, `curl`). Lists of same-shaped objects become `{"_cols":[…],"_rows":[[…]]}`, which is lossless, and JSON is minified with escapes written as plain characters. Link-noise keys (`*_url` except `html_url`/`url`, `node_id`, `_links`) are dropped, recoverably, since the raw response is logged; turn that off with `x config json_prune false`. Measured on a GitHub list call: −27% for 3 items, and the saving grows with list length. |
| **`t`** smart test | Detects pytest / jest / vitest / go / cargo. Reruns the last failures (`--lf`, `--onlyFailures`, `-run`, name filters), then runs the full suite once they pass. With no previous failures, it runs only tests related to changed files (`vitest related`, `jest --findRelatedTests`, changed Go packages, `test_<module>.py`). Repeat runs show only what changed: `[Δ 2 fixed · 1 still]`, and an unchanged failure takes one line. |
| **`c`** check | Typecheck, lint, vet and clippy for the nearest project, run in parallel. It respects the project's own lint config; without one, ruff runs only its correctness rules, because its built-in defaults now include style rules. Errors are grouped by file and repeated messages merged. Errors inside `node_modules`/`site-packages` collapse to one line, since they're usually a config problem. |
| **`o`** navigate | `o file` → symbols with line ranges · `o dir` → repo map · `o def X` → definition body · `o refs X` → references by file · `o def zod.object` → looks inside installed dependencies (node / python / go / cargo, `.d.ts` first). |
| **`g`** git | One-line status, diff with tight context and lockfiles collapsed, short log. |
| **`sub`** | Search and replace across files as a dry-run diff, then `--apply`. It understands sed-style `\n`/`\t`, handles patterns that span lines, and runs the edit check on every file it changes. Replaces dozens of Edit calls. |
| **Read guard** (PreToolUse) | A full Read of a code file over 400 lines (markdown: 800) is blocked once, and the outline is returned instead. Reading the same file again goes through. Skill, plugin and `.claude` directories, and `SKILL.md`/`CLAUDE.md`/`AGENTS.md`, are never guarded, because the agent was told to read them in full. Follow-up partial reads and full re-reads are counted as costs, so the reported savings stay honest. |
| **Edit check** (PostToolUse) | After each Edit or Write, a check of under a second for syntax errors and undefined names: `ast`/ruff, `node --check`, esbuild, gofmt, rustfmt. It's silent on success and reports one line on failure, which catches mistakes before a test run. |
| **Session brief** (SessionStart / SubagentStart) | About 4 lines covering repo, branch, stack, test and check commands, top directories, and the tools. It replaces the usual 3–5 exploratory calls. It's injected once per session, so the prompt cache stays intact. |
| **Task state** (SessionStart on resume or compaction, PreCompact) | Files edited, last test result and failing tests, and last check result. After compaction the agent gets this back in a few lines and doesn't have to rediscover it. |
| **Quiet environment** | `NO_COLOR`, no pagers, no npm fund/update/progress output, no pip version check, no cargo progress bar. These are exported through `CLAUDE_ENV_FILE`. |

### Failed commands

Claude Code doesn't let hooks rewrite the output of a **failed** call; it passes it to the model unchanged. So for anything that may fail, the brief and the skill steer the agent to `t`, `c` or `x run <cmd>`. These filter inside the command, so a failing run is compact too. The failure hook records missed savings (they appear in `x stats`) and gives a one-line tip at most once per session.

Automatic command rewriting (PreToolUse `updatedInput` → `lean run …`) is **on only in `bypassPermissions` mode**. In other modes, permission rules and the auto-mode classifier review the command's input, and rewriting it would either trigger prompts or get denied. Set `x config rewrite always` to force it on, or `never` to turn it off.

## Measurement

**Actual usage** comes from Claude Code transcripts: per-request input, cache write (5m/1h), cache read and output tokens, de-duplicated by message ID, plus subagent transcripts. These are priced per model, with the table in `lib/lean/usage.py`. You can override it with `"prices"` in `~/.lean/config.json`.

**Savings** are measured, not estimated. The hook sees the raw output and what was shown, and logs both.

**Carried savings.** A tool result is re-sent with every later request until the next compaction. lean counts `tokens × later requests` and prices it the way Claude Code is billed: one cache write plus a cache read per later request.

**Subscription plans (Pro/Max).** The status line reads `rate_limits.five_hour` / `seven_day` and shows `5h 42% (≈50% w/o lean)` and `×1.19 capacity`. The "without lean" figure assumes plan usage scales with API-equivalent cost, so it's marked `≈`.

**Tokens per character** default to 3.6. `x calibrate` fits the ratio from your own transcripts.

```
⛁ 15.7M in (97% cached) · 156k out · $7.94 · ctx 31% │ lean saved 54.9k tok · $0.07 (1%)
⛁ 5h 40% (≈44% w/o lean) · 7d 12% · 15.8M tok · ctx 31% │ lean saved … · ×1.10 capacity
```

`x stats [session|today|week|5h|all]` shows usage, savings by source, missed savings and plan windows. `x waste` lists the largest tool outputs still entering context, grouped by tool and command, so you know which rule to add next.

## Install

**Local Claude Code (any OS with bash + python3):**
```bash
git clone <this repo> ~/lean && ~/lean/install.sh      # links t c o g sub x into ~/.local/bin,
                                                       # adds hooks + status line to ~/.claude/settings.json
~/lean/install.sh --no-status                          # keep your own status line
~/lean/install.sh --uninstall
```
**As a plugin:** `claude plugin marketplace add <owner>/<repo>`, then `claude plugin install lean@lean`. The plugin's `bin/` goes on PATH automatically. Plugins can't set the status line, so point `statusLine.command` at `<plugin dir>/bin/lean statusline` yourself.

**Cloud sessions (claude.ai/code):** add this repo to the environment, and put this in the environment's setup script: `bash <repo-dir>/install.sh`.

## Config (`x config key value`)

| key | default | |
|---|---|---|
| `billing` | auto | `api` / `subscription`. Auto picks api if `ANTHROPIC_API_KEY` is set. |
| `read_guard_lines` | 400 | `0` disables the read guard. |
| `edit_check` | true | Per-edit syntax check. |
| `rewrite` | auto | `always` / `never`. Auto means only in bypassPermissions mode. |
| `quiet_env` | true | Export quiet variables at session start. |
| `chars_per_token` | 3.6 | Set by `x calibrate`. |
| `json_prune` | true | Drop link-noise keys from large JSON. The raw response is always logged. |
| `debug` | false | Logs hook payloads to `~/.lean/debug/`. |

## Skills

Skill content is instructions, so lean never compresses it. Skills still benefit indirectly: their scripts' output, MCP calls and test runs go through the same filters. `x waste` shows what each tool, MCP server and skill load costs, so a new heavy source shows up right away.

## Tests

`python3 -m unittest discover -s tests`. The fixtures are real outputs from pytest, jest, vitest, go test, cargo test, tsc, eslint, ruff and rustc. Each test checks that every key fact survives (test names, `file:line`, error codes and messages) and that the output actually shrinks.
