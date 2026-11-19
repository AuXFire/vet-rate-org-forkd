---
name: lean
description: Token-lean tools for coding work - t (smart tests), c (typecheck+lint), o (outline/defs/refs incl. dependencies), g (compact git), sub (find/replace), x stats. Use when running tests or checks, exploring code, or checking token savings.
---

Bash output is filtered automatically (failures in full; passing runs as one line). Every filtered result ends with a log path that holds the raw output; grep that log instead of rerunning. Prefix `RAW=1` to skip filtering.

| Need | Use | Instead of |
|---|---|---|
| Run tests | `t` reruns last failures, then the full suite once they pass; otherwise runs tests related to changed files. `t all`, `t <args>` | `npm test`, `pytest`, rerunning the whole suite |
| Typecheck + lint | `c` (all detected checks in parallel, errors grouped) | separate tsc / eslint / ruff / vet runs |
| Understand a file | `o file.ts` → symbols with line ranges, then Read with offset/limit | reading the whole file |
| Find code | `o def Name`, `o refs Name`, `o file.py Sym` (body only) | grep + reading files |
| Library API | `o def zod.object`, `o def requests.get` (reads installed deps, .d.ts first) | opening node_modules / site-packages |
| Repo overview | `o .` or `o src/` | ls -R, tree, many Reads |
| Git | `g` status · `g d` diff (lockfiles collapsed) · `g l` log | git status / diff / log |
| Rename / replace | `sub old new [paths] [-w] [-r]` (dry-run diff) then `--apply` | many Edit calls |
| Savings | `x stats [session\|today\|week]`, `x waste` | |

Notes:
- If a full Read of a large file is blocked with an outline, read the range you need. Reading the same file again without offset returns all of it.
- An `edit-check ✗` note after an edit means your edit was saved but has a syntax or undefined-name error. Fix that error first.
- In `t` output, `Δ 2 fixed · 1 still · 1 new` compares this run with the previous one, and `= … unchanged` marks failures whose message hasn't changed.
