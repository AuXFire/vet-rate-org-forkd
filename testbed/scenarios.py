"""Scenario catalogue. Each scenario = project + file overrides + command (+ explicit facts).

facts:  strings that MUST survive filtering (checked in addition to the independent oracles)
expect: "fail" | "pass" — ground truth used for the false-success check
"""
import json


def S(name, project, cmd, files=None, facts=(), expect="fail", note=""):
    return dict(name=name, project=project, cmd=cmd, files=files or {}, facts=list(facts), expect=expect, note=note)


# ---------------------------------------------------------------- generated content
def py_big_suite(n=300, fail_every=5):
    return (
        "import pytest\n"
        "from calc.core import add\n\n"
        "@pytest.mark.parametrize('i', range(%d))\n"
        "def test_many(i):\n"
        "    if i %% %d == 0:\n"
        "        assert add(i, 0) == -1, 'sentinel failure'\n"
        "    if i %% 97 == 1:\n"
        "        raise RuntimeError('boom %%d' %% (i // 97))\n"
        "    assert True\n" % (n, fail_every)
    )


PY_FIXED = {
    "calc/core.py": "def add(a, b):\n    return a + b\n\n\ndef name(user):\n    return user.get('name', user.get('nm', '')).strip()\n\n\ndef ratio(a, b):\n    return a / b if b else 0\n",
    "tests/test_core.py": "from calc.core import add, name\n\n\ndef test_add():\n    assert add(1, 2) == 3\n\n\ndef test_name():\n    assert name({'nm': ' x '}) == 'x'\n",
}

VT_BIG = (
    "import { test, expect } from 'vitest';\n"
    "import { sum } from './sum';\n"
    "for (let i = 0; i < 200; i++) test(`case ${i}`, () => { if (i % 5 === 0) expect(sum(i, 0)).toBe(-1); expect(true).toBe(true); });\n"
)

API_JSON_SCRIPT = (
    "import json\n"
    "rows = [{'id': i, 'login': 'user%d' % i, 'node_id': 'MDQ6%06d' % i, 'avatar_url': 'https://avatars.example.com/u/%d' % i,\n"
    "         'url': 'https://api.example.com/users/%d' % i, 'html_url': 'https://example.com/user%d' % i,\n"
    "         'followers_url': 'https://api.example.com/users/%d/followers' % i, 'repos_url': 'https://api.example.com/users/%d/repos' % i,\n"
    "         'type': 'User', 'site_admin': False, 'score': 1.0} for i in range(120)]\n"
    "print(json.dumps({'total_count': 120, 'items': rows}, indent=2))\n"
)

PROGRESS_SCRIPT = (
    "import sys, time\n"
    "for i in range(400):\n"
    "    sys.stdout.write('\\rDownloading model shards: %3d%% [%s%s]' % (i // 4, '#' * (i // 20), '.' * (20 - i // 20)))\n"
    "print()\n"
    "for i in range(300):\n"
    "    print('INFO 2026-11-19T10:%02d:%02d worker-%d processed batch %d in %dms' % (i // 60, i % 60, i % 4, i, 20 + i % 7))\n"
    "print('ERROR worker-2 batch 271 failed: ConnectionResetError: [Errno 104] Connection reset by peer')\n"
    "for i in range(100):\n"
    "    print('INFO tail line %d' % i)\n"
    "sys.exit(0)\n"
)

LOCK_DIFF_SETUP = "git init -q . && git add -A && git -c user.email=t@t -c user.name=t commit -qm init && python3 gen_lock.py && sed -i 's/a + b + (1 if a > 5 else 0)/a + b/' calc/core.py"
GEN_LOCK = "import json\njson.dump({'packages': {'pkg%d' % i: {'version': '2.%d.0' % i, 'integrity': 'sha512-%064d' % i} for i in range(400)}}, open('package-lock.json', 'w'), indent=2)\n"

SCENARIOS = [
    # ------------------------------------------------ python
    S("py-fail", "py", "python3 -m pytest",
      facts=["test_add_big", "test_name", "KeyError: 'name'", "test_compare", "test_pct_zero", "ZeroDivisionError", "calc/core.py:6", "calc/core.py:10"]),
    S("py-fail-verbose", "py", "python3 -m pytest -v", facts=["test_add_big", "ZeroDivisionError", "calc/core.py:10"]),
    S("py-fail-longtb", "py", "python3 -m pytest --tb=long -vv", facts=["test_compare", "ZeroDivisionError"]),
    S("py-pass", "py", "python3 -m pytest", files=PY_FIXED, expect="pass"),
    S("py-collect-error", "py", "python3 -m pytest", files={"tests/test_fmt.py": "from calc.fmt import pct\n\ndef test_pct(:\n    pass\n"},
      facts=["test_fmt.py", "SyntaxError"]),
    S("py-import-error", "py", "python3 -m pytest", files={"calc/fmt.py": "from calc.core import ratio, missing_fn\n"},
      facts=["test_fmt.py", "missing_fn", "ImportError"]),
    S("py-big", "py", "python3 -m pytest", files={"tests/test_big.py": py_big_suite()},
      facts=["sentinel failure", "RuntimeError", "boom 0", "test_many", "test_name", "ZeroDivisionError"]),
    S("py-big-pass-verbose", "py", "python3 -m pytest -v", files=dict(PY_FIXED, **{"tests/test_big.py": "import pytest\n\n@pytest.mark.parametrize('i', range(500))\ndef test_many(i):\n    assert i >= 0\n"}),
      expect="pass"),
    S("py-warnings", "py", "python3 -m pytest", files=dict(PY_FIXED, **{"tests/test_w.py": "import warnings\n\ndef test_w():\n    for i in range(30):\n        warnings.warn('old api %d' % i, DeprecationWarning)\n"}),
      expect="pass", facts=["30 warnings", "DeprecationWarning", "old api 0", "old api 29", "tests/test_w.py:5"]),
    S("py-ruff", "py", "ruff check --output-format concise --select E,F,W,B .", files={"calc/bad.py": "import os, sys\n\ndef f(x=[]):\n    return undefined_name + x  \n" + "".join("v%d = 1 ;\n" % i for i in range(40))},
      facts=["F401", "F821", "B006", "calc/bad.py:4"]),
    S("py-ruff-full", "py", "ruff check --select E,F,B .", files={"calc/bad.py": "import os, sys\n\ndef f(x=[]):\n    return undefined_name + x\n"},
      facts=["F401", "F821", "B006"]),
    # ------------------------------------------------ vitest / ts
    S("vt-fail", "vitest", "npx vitest run", facts=["big 6", "big 8", "user", "objects", "src/sum.ts:4", "expected 8 to be 7"]),
    S("vt-pass", "vitest", "npx vitest run", files={"src/sum.ts": "export const sum = (a: number, b: number): number => a + b;\nexport interface User { profile?: { name: string } }\nexport function userName(u: User): string { return u.profile?.name ?? 'x'; }\n",
                                                    "src/sum.test.ts": "import { test, expect } from 'vitest';\nimport { sum } from './sum';\ntest('a', () => expect(sum(1, 2)).toBe(3));\n"},
      expect="pass", facts=["1 passed"]),
    S("vt-suite-error", "vitest", "npx vitest run", files={"src/extra.test.ts": "import { nope } from './missing';\nimport { test } from 'vitest';\ntest('x', () => nope());\n"},
      facts=["extra.test.ts", "missing"]),
    S("vt-big", "vitest", "npx vitest run", files={"src/big.test.ts": VT_BIG}, facts=["case 0", "case 195", "to be -1"]),
    S("tsc-errors", "vitest", "npx tsc --noEmit", files={"src/bad.ts": "export function f(x) { return x * 2 }\nconst n: number = 'str';\nexport const k = (q: string): number => q;\n" + "".join("export const v%d: string = %d;\n" % (i, i) for i in range(30))},
      facts=["src/bad.ts", "TS7006", "TS2322", "1:", "33"]),
    S("tsc-pretty", "vitest", "npx tsc --noEmit --pretty", files={"src/bad.ts": "const n: number = 'str';\nexport const k = (q: string): number => q;\n"},
      facts=["src/bad.ts", "TS2322"]),
    S("eslint-errors", "vitest", "npx eslint src", files={"eslint.config.js": "export default [{ files: ['**/*.ts', '**/*.js'], rules: { 'no-unused-vars': 'error', 'no-undef': 'error', eqeqeq: 'warn' } }];\n",
                                                        "src/lint.js": "const a = 1; const b = 2;\nif (x == 1) { y = 2 }\n" + "".join("const u%d = %d;\n" % (i, i) for i in range(25))},
      facts=["src/lint.js", "no-unused-vars", "no-undef", "eqeqeq"]),
    S("vt-npm-install-verbose", "vitest", "npm install --no-audit --no-fund --loglevel=http left-pad@1.3.0", expect="pass", facts=["left-pad"]),
    # ------------------------------------------------ jest
    S("jest-fail", "jest", "npx jest", facts=["big 6", "big 8", "user", "Expected: 7", "Received: 8", "sum.js:2"]),
    S("jest-fail-verbose", "jest", "npx jest --verbose", facts=["big 6", "user"]),
    # ------------------------------------------------ go
    S("go-fail", "go", "go test ./...", facts=["TestAddBig", "Add(6,1) = 7; want 7"[:0] + "want 7", "calc_test.go:15"]),
    S("go-fail-verbose", "go", "go test -v ./...", facts=["TestAddBig", "want 7", "want 8", "want 9"]),
    S("go-panic", "go", "go test ./...", files={"calc_test.go": "package calc\n\nimport \"testing\"\n\nfunc TestName(t *testing.T) {\n\tif Name(map[string]*string{}) != \"x\" {\n\t\tt.Fatal(\"x\")\n\t}\n}\n"},
      facts=["TestName", "nil pointer dereference", "calc.go:10"]),
    S("go-build-error", "go", "go test ./...", files={"calc.go": "package calc\n\nfunc Add(a, b int) int { return a + b + undefinedThing }\n\nfunc Name(m map[string]*string) string { return *m[\"name\"] }\n"},
      facts=["undefined: undefinedThing", "calc.go:3"]),
    S("go-vet", "go", "go vet ./...", files={"util/util.go": "package util\n\nimport \"fmt\"\n\nfunc Double(x int) int { fmt.Printf(\"%d %s\\n\", x); return x * 2 }\n"},
      facts=["util.go:5", "Printf"]),
    S("go-pass-verbose", "go", "go test -v ./...", files={"calc.go": "package calc\n\nfunc Add(a, b int) int { return a + b }\n\nfunc Name(m map[string]*string) string { return \"\" }\n"},
      expect="pass", facts=["ok"]),
    # ------------------------------------------------ rust
    S("rust-fail", "rust", "cargo test", facts=["tests::big", "tests::big2", "tests::empty", "big2 case", "index out of bounds", "src/lib.rs:6", "left: 8"]),
    S("rust-compile-error", "rust", "cargo build", files={"src/main.rs": "fn main() { let x: i32 = \"s\"; let y = undefined_fn(); println!(\"{}\", x); }\n"},
      facts=["E0308", "E0425", "src/main.rs:1"]),
    S("rust-warnings", "rust", "cargo build", files={"src/main.rs": "fn main() {\n" + "".join("    let v%d = %d;\n" % (i, i) for i in range(40)) + "    println!(\"hi\");\n}\n"},
      expect="pass", facts=["unused variable"]),
    S("rust-clippy", "rust", "cargo clippy --all-targets", files={"src/main.rs": "fn main() { let v = vec![1, 2]; if v.len() == 0 { println!(\"e\"); } let x = 5; let y = x.clone(); println!(\"{}\", y); }\n"},
      expect="pass", facts=["len_zero", "clone_on_copy"]),
    # ------------------------------------------------ jvm / ruby / c
    S("maven-fail", "maven", "mvn -B test", facts=["CalcTest.big", "CalcTest.name", "expected: <7> but was: <8>", "NullPointerException", "Calc.java:9"]),
    S("maven-compile-error", "maven", "mvn -B -q compile", files={"src/main/java/demo/Calc.java": "package demo;\npublic class Calc {\n    public static int add(int a, int b) { return a + b + c; }\n}\n"},
      facts=["Calc.java", "cannot find symbol"]),
    S("rspec-fail", "ruby", "rspec spec", facts=["adds big 6", "adds big 8", "names", "KeyError", "calc.rb:7"]),
    S("minitest-fail", "ruby", "ruby -Ilib test/calc_test.rb", facts=["test_big", "test_name", "KeyError", "calc_test.rb:10"]),
    S("c-make-errors", "c", "make", files={"calc.c": "#include \"calc.h\"\nint add(int a, int b) { int unused; return a + b + undeclared_var; }\nchar *s(void) { return 5; }\n"},
      facts=["calc.c:2", "undeclared_var", "calc.c:3"]),
    # ------------------------------------------------ generic
    S("json-api", "py", "python3 api.py", files={"api.py": API_JSON_SCRIPT}, expect="pass", facts=["user119", "https://example.com/user0", "total_count"]),
    S("progress-log", "py", "python3 prog.py", files={"prog.py": PROGRESS_SCRIPT}, expect="pass", facts=["ConnectionResetError", "batch 271", "tail line 99"]),
    S("cat-big", "py", "cat big.txt", files={"big.txt": "".join("line %d of the file\n" % i for i in range(3000))}, expect="pass", facts=["line 2999 of the file", "line 1500 of the file"]),
    S("lock-diff", "py", "git diff", files={"gen_lock.py": GEN_LOCK, "package-lock.json": "{}\n"}, expect="pass", facts=["a + b", "package-lock.json"]),
    S("git-clone", "py", "git clone --progress https://github.com/octocat/Hello-World.git hw", expect="pass", facts=["Cloning into"]),
]

SETUP_CMDS = {"lock-diff": LOCK_DIFF_SETUP}
