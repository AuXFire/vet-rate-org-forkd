"""Fixture tests: every key fact must survive filtering, and output must shrink.

Run: python3 -m unittest discover -s tests -v
"""
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
os.environ["LEAN_HOME"] = tempfile.mkdtemp(prefix="lean-test-")
from lean import filt, hook, nav, parse, usage, util  # noqa: E402

FX = os.path.join(HERE, "fixtures")
PY = os.path.join(FX, "pyproj")


def fx(name):
    with open(os.path.join(FX, name)) as f:
        return f.read()


class KeyFacts(unittest.TestCase):
    """(command, fixture, cwd, facts that must appear, max ratio shown/raw)"""
    CASES = [
        ("python -m pytest", "pytest.txt", PY,
         ["5 failed", "2 passed", "test_add_big[6|7|8|9]", "test_name", "KeyError: 'name'", "calc.py:5", "assert 8 == (6 + 1)"], 0.4),
        ("cargo test", "cargotest.txt", None,
         ["2 failed", "1 passed", "tests::big", "tests::big2", "src/lib.rs:6", "src/lib.rs:7", "left: 8", "right: 7", "big2 case"], 0.35),
        ("npx vitest run", "vitest.txt", None,
         ["4 failed", "1 passed", "big 6", "big 7", "big 8", "user", "expected 8 to be 7", "reading 'name'", "sum.test.js:4", "sum.test.js:5"], 0.4),
        ("npx vitest run", "vitest_suite.txt", None,
         ["2 files failed", "jt/sum.test.cjs (suite)", "ReferenceError: test is not defined", "sum.js:2", "big 8", "expected 8 to be 7"], 0.4),
        ("npx jest", "jest.txt", None,
         ["4 failed", "1 passed", "big 6", "big 8", "user", "Expected: 7", "Received: 8", "reading 'name'"], 0.3),
        ("npx tsc --noEmit", "tsc.txt", None,
         ["5 errors", "13 issues inside dependencies", "src/a.ts", "1:19 TS7006", "4:7 TS2322", "5:41 TS2322", "TS2792", "undici-types"], 0.5),
        ("npx tsc --noEmit", "tsc_pretty.txt", None, ["5 errors", "13 issues", "4:7 TS2322"], 0.5),
        ("python -m pytest -v", "pytest_pass_verbose.txt", None, ["✓ pytest", "300 passed"], 0.01),
    ]

    def test_cases(self):
        for cmd, f, cwd, facts, ratio in self.CASES:
            with self.subTest(f):
                raw = fx(f)
                util.save_state(util.root(cwd or FX), {})          # no Δ from previous runs
                out, kind = filt.filter_output(cmd, raw, cwd or FX, code=1 if "pass" not in f else 0)
                full = out + "\n" + filt.expand_braces(out)
                for fact in facts:
                    self.assertTrue(fact in full or fact.split("::")[-1] in full, "%s lost %r\n---\n%s" % (f, fact, out))
                self.assertLessEqual(len(out), len(raw) * ratio, "%s: %d -> %d chars\n%s" % (f, len(raw), len(out), out))


class Diagnostics(unittest.TestCase):
    def test_eslint(self):
        d = parse.diagnostics(fx("eslint.txt"))
        self.assertEqual(len([x for x in d if x["s"] == "error"]), 7)
        self.assertEqual(len([x for x in d if x["s"] == "warning"]), 2)

    def test_rust(self):
        d = parse.diagnostics(fx("cargobuild.txt"))
        self.assertEqual([(x["code"], x["l"], x["c"]) for x in d], [("E0308", 1, 26), ("E0425", 1, 39)])

    def test_ruff_full(self):
        d = parse.diagnostics(fx("ruff.txt"))
        self.assertEqual(sorted(x["code"] for x in d), ["F401", "F401", "F821", "I001"])

    def test_go(self):
        d = parse.diagnostics(fx("gobuild.txt"))
        self.assertEqual(d[0]["m"], "undefined: undefinedThing")

    def test_build_success_with_warnings(self):
        out = filt.build(fx("cargo_warn.txt"), "cargo build", code=0)
        self.assertTrue(out.startswith("✓"), out)
        self.assertIn("unused variable", out)

    def test_build_errors_not_reported_as_success(self):
        out, _ = filt.filter_output("cargo build", fx("cargobuild.txt") * 3, FX, code=None)
        self.assertIn("E0308", out)
        self.assertNotIn("✓", out)


class Generic(unittest.TestCase):
    def test_repeats_and_budget(self):
        raw = "\n".join(["start"] + ["Downloading chunk %d of 900" % i for i in range(900)] + ["ERROR: disk full at /var/x"] + ["tail %d" % i for i in range(300)])
        out = filt.generic(raw)
        self.assertIn("ERROR: disk full at /var/x", out)
        self.assertIn("tail 299", filt.expand_braces(out))
        self.assertLess(len(out), len(raw) * 0.25)

    def test_json_compaction_is_lossless(self):
        from lean import jsonc
        obj = {"items": [{"id": i, "name": "n%d" % i, "tags": ["a", "b"], "meta": {"x": None}} for i in range(60)], "next": "é<>"}
        raw = json.dumps(obj, indent=2)
        out = filt.generic(raw)
        body, note = out.rsplit("\n", 1)
        self.assertEqual(jsonc.expand(json.loads(body)), obj)
        self.assertLess(len(out), len(raw) * 0.4)
        self.assertIn("_cols/_rows", note)

    def test_read_commands_untouched(self):
        raw = "x\n" * 5000
        for cmd in ("cat big.py", "sed -n 1,400p a.py", "head -500 f", "cd x && cat y"):
            out, kind = filt.filter_output(cmd, raw, FX)
            self.assertEqual(out, raw, cmd)

    def test_pipes_only_squeezed(self):
        self.assertEqual(filt.kind_of("pytest | tail -5"), "pipe")
        self.assertEqual(filt.kind_of("RAW=1 pytest"), "raw")
        self.assertEqual(filt.kind_of("cd a && FOO=1 npm test 2>&1"), "test")
        self.assertEqual(filt.kind_of("uv run pytest -x"), "test")

    def test_install_success_one_line(self):
        raw = "\n".join(["npm http fetch GET 200 https://registry.npmjs.org/p%d 12ms" % i for i in range(400)] + ["", "added 412 packages, and audited 413 packages in 8s", "", "found 0 vulnerabilities"])
        out, k = filt.filter_output("npm install", raw, FX, code=0)
        self.assertEqual(k, "install")
        self.assertIn("added 412 packages", out)
        self.assertIn("found 0 vulnerabilities", out)
        self.assertEqual(out.count("\n"), 0)

    def test_install_failure_keeps_errors(self):
        raw = "\n".join(["npm http fetch GET 200 https://registry.npmjs.org/p%d" % i for i in range(300)] + ["npm ERR! code ERESOLVE", "npm ERR! Could not resolve dependency: peer react@18"])
        out, _ = filt.filter_output("npm install", raw, FX, code=1)
        self.assertIn("ERESOLVE", out)
        self.assertIn("peer react@18", out)

    def test_diff_collapses_lockfiles_only(self):
        raw = "diff --git a/src/a.py b/src/a.py\n--- a/src/a.py\n+++ b/src/a.py\n@@ -1 +1 @@\n-old\n+new\n" + \
              "diff --git a/package-lock.json b/package-lock.json\n--- a/package-lock.json\n+++ b/package-lock.json\n@@ -1,900 +1,900 @@\n" + \
              "\n".join("-    \"x%d\": \"1\"\n+    \"x%d\": \"2\"" % (i, i) for i in range(900))
        out, _ = filt.filter_output("git diff", raw, FX)
        self.assertIn("+new", out)
        self.assertIn("package-lock.json: +900 −900", out)
        self.assertLess(len(out), 600)


class Delta(unittest.TestCase):
    def test_unchanged_failures_compress(self):
        import shutil
        d = os.path.join(tempfile.mkdtemp(), "p")
        shutil.copytree(PY, d)
        raw = fx("pytest.txt")
        first, _ = filt.filter_output("pytest", raw, d, code=1)
        second, _ = filt.filter_output("pytest", raw, d, code=1)
        self.assertIn("[Δ 5 still]", second)
        self.assertIn("unchanged", second)
        self.assertLess(len(second), len(first) * 0.6)


class Nav(unittest.TestCase):
    def test_python_outline_ranges(self):
        syms = nav.outline(os.path.join(PY, "calc.py"))
        self.assertEqual([(s[4], s[0], s[1]) for s in syms], [("add", 1, 2), ("name", 4, 5)])

    def test_ts_outline(self):
        p = os.path.join(tempfile.mkdtemp(), "a.ts")
        open(p, "w").write("export interface U {\n  a: string\n}\nexport class S {\n  run(x: number): void {\n    if (x) {\n      go()\n    }\n  }\n}\nexport const f = async (a: string) => {\n  return a\n}\n")
        got = [(s[4], s[0], s[1], s[2]) for s in nav.outline(p)]
        self.assertEqual(got, [("U", 1, 3, 0), ("S", 4, 10, 0), ("run", 5, 9, 1), ("f", 11, 13, 0)])

    def test_go_rust_outline(self):
        d = tempfile.mkdtemp()
        open(os.path.join(d, "a.go"), "w").write("package m\n\ntype T struct {\n  A int\n}\n\nfunc (t *T) Run(x int) error {\n  return nil\n}\n")
        open(os.path.join(d, "a.rs"), "w").write("pub struct S {\n    a: i32,\n}\nimpl S {\n    pub fn new() -> Self {\n        S { a: 1 }\n    }\n}\n")
        self.assertEqual([(s[4], s[0], s[1]) for s in nav.outline(os.path.join(d, "a.go"))], [("T", 3, 5), ("Run", 7, 9)])
        self.assertEqual([(s[4], s[0], s[1]) for s in nav.outline(os.path.join(d, "a.rs"))], [("S", 1, 3), ("S", 4, 8), ("new", 5, 7)])


class Hooks(unittest.TestCase):
    def run_hook(self, ev):
        buf = io.StringIO()
        old = sys.stdin
        sys.stdin = io.StringIO(json.dumps(ev))
        try:
            with redirect_stdout(buf):
                hook.main()
        finally:
            sys.stdin = old
        return json.loads(buf.getvalue()) if buf.getvalue().strip() else None

    def test_post_bash_replaces_output(self):
        raw = fx("pytest_pass_verbose.txt")
        r = self.run_hook({"hook_event_name": "PostToolUse", "tool_name": "Bash", "session_id": "s1", "cwd": FX,
                           "tool_input": {"command": "python -m pytest -v"},
                           "tool_response": {"stdout": raw, "stderr": "", "interrupted": False, "isImage": False}})
        new = r["hookSpecificOutput"]["updatedToolOutput"]
        self.assertIn("300 passed", new["stdout"])
        self.assertIn("raw: ", new["stdout"])
        self.assertEqual(set(new), {"stdout", "stderr", "interrupted", "isImage"})
        log = new["stdout"].split("raw: ")[1].rstrip("]")
        self.assertEqual(open(os.path.expanduser(log)).read(), raw)   # lossless recovery

    def test_post_bash_leaves_reads_alone(self):
        r = self.run_hook({"hook_event_name": "PostToolUse", "tool_name": "Bash", "session_id": "s1", "cwd": FX,
                           "tool_input": {"command": "cat big.txt"}, "tool_response": {"stdout": "line\n" * 4000, "stderr": ""}})
        self.assertIsNone(r)

    def test_read_guard_deny_then_allow(self):
        p = os.path.join(tempfile.mkdtemp(), "big.py")
        body = "".join("    y = x * %d  # step %d of the computation\n" % (k, k) for k in range(10))
        with open(p, "w") as fh:
            fh.write("\n".join("def f%d(x):\n%s    return y + %d\n" % (i, body, i) for i in range(60)))
        ev = {"hook_event_name": "PreToolUse", "tool_name": "Read", "session_id": "s2", "tool_input": {"file_path": p}}
        r1 = self.run_hook(ev)
        self.assertEqual(r1["hookSpecificOutput"]["permissionDecision"], "deny")
        self.assertIn("def f59(x)", r1["hookSpecificOutput"]["permissionDecisionReason"])
        self.assertIsNone(self.run_hook(ev))          # retry passes through
        r3 = self.run_hook(dict(ev, tool_input={"file_path": p, "offset": 10, "limit": 20}))
        self.assertIsNone(r3)

    def test_edit_check_flags_syntax_error(self):
        p = os.path.join(tempfile.mkdtemp(), "x.py")
        open(p, "w").write("def f(:\n  pass\n")
        r = self.run_hook({"hook_event_name": "PostToolUse", "tool_name": "Edit", "session_id": "s3", "tool_input": {"file_path": p}})
        self.assertIn("SyntaxError", r["hookSpecificOutput"]["additionalContext"])
        open(p, "w").write("def f():\n  pass\n")
        self.assertIsNone(self.run_hook({"hook_event_name": "PostToolUse", "tool_name": "Edit", "session_id": "s3", "tool_input": {"file_path": p}}))

    def test_session_start_brief_and_env(self):
        envf = tempfile.mktemp()
        os.environ["CLAUDE_ENV_FILE"] = envf
        try:
            r = self.run_hook({"hook_event_name": "SessionStart", "session_id": "s4", "cwd": PY, "source": "startup"})
        finally:
            del os.environ["CLAUDE_ENV_FILE"]
        ctx = r["hookSpecificOutput"]["additionalContext"]
        self.assertIn("[lean]", ctx)
        self.assertIn("tools:", ctx)
        self.assertLess(len(ctx), 900)
        self.assertIn("export NO_COLOR=1", open(envf).read())

    def test_mcp_json_minified(self):
        obj = {"rows": [{"k": i, "v": "x" * 10} for i in range(80)]}
        r = self.run_hook({"hook_event_name": "PostToolUse", "tool_name": "mcp__db__query", "session_id": "s5",
                           "tool_response": [{"type": "text", "text": json.dumps(obj, indent=2)}]})
        out = r["hookSpecificOutput"]["updatedMCPToolOutput"]
        from lean import jsonc
        self.assertEqual(jsonc.expand(json.loads(out[0]["text"])), obj)      # lossless
        self.assertIn("raw:", out[-1]["text"])

    def test_mcp_string_response_and_prune(self):
        obj = [{"login": "u%d" % i, "id": i, "avatar_url": "https://a/%d" % i, "html_url": "https://h/%d" % i, "node_id": "N%d" % i} for i in range(30)]
        r = self.run_hook({"hook_event_name": "PostToolUse", "tool_name": "mcp__gh__users", "session_id": "s6", "tool_response": json.dumps(obj)})
        out = r["hookSpecificOutput"]["updatedMCPToolOutput"]
        from lean import jsonc
        body, note = out.rsplit("\n", 1)
        self.assertEqual(jsonc.expand(json.loads(body)), [{"login": o["login"], "id": o["id"], "html_url": o["html_url"]} for o in obj])
        self.assertIn("dropped 60", note)


class Usage(unittest.TestCase):
    def test_carried_savings(self):
        rows = [[100.0 + i, "claude-opus-5-5", 0, 0, 0, 1000, 10, ""] for i in range(10)]
        m = [{"ts": 104.5, "raw": 36000, "shown": 0, "kind": "bash:test"}]
        sv = usage.savings(m, rows)
        tok = util.tokens(36000)
        self.assertEqual(sv["direct"], tok)
        self.assertEqual(sv["carried"], tok * 5)                 # 5 later requests
        self.assertAlmostEqual(sv["usd"], tok * (4 * 2 + 4 * 0.20) / 1e6)

    def test_compaction_stops_carry(self):
        rows = [[100.0 + i, "claude-opus-5-5", 0, 0, 0, 1000, 10, ""] for i in range(10)]
        sv = usage.savings([{"ts": 101.5, "raw": 3600, "shown": 0}], rows, compacts=[104.2])
        self.assertEqual(sv["carried"], util.tokens(3600) * 3)   # requests at 102,103,104 only

    def test_price_lookup(self):
        self.assertEqual(usage.price("claude-opus-5-5"), (4, 20, 0.20))
        self.assertEqual(usage.price("claude-sonnet-5-5"), (2, 10, 0.10))
        self.assertEqual(usage.price("claude-fable-5-2"), (8, 40, 0.20))


if __name__ == "__main__":
    unittest.main()
