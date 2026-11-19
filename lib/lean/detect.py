"""Project detection: which stacks live here and how to test / check them."""
import json
import os
import shutil
from . import util

MANIFESTS = ("package.json", "pyproject.toml", "setup.py", "setup.cfg", "pytest.ini", "tox.ini", "go.mod", "Cargo.toml", "requirements.txt", "conftest.py")


def _has(d, *names):
    return any(os.path.exists(os.path.join(d, n)) for n in names)


def project_dir(cwd=None):
    """Nearest dir (cwd upward, not above repo root) that has a manifest."""
    cwd = os.path.abspath(cwd or os.getcwd())
    top = util.root(cwd)
    d = cwd
    while True:
        if _has(d, *MANIFESTS):
            return d
        if d == top or os.path.dirname(d) == d:
            return cwd
        d = os.path.dirname(d)


def _pkg(d):
    try:
        with open(os.path.join(d, "package.json")) as f:
            return json.load(f)
    except Exception:
        return {}


def _bin(d, name):
    p = os.path.join(d, "node_modules", ".bin", name)
    if os.path.exists(p):
        return p
    r = util.root(d)
    p = os.path.join(r, "node_modules", ".bin", name)
    if os.path.exists(p):
        return p
    return name if shutil.which(name) else None


def pm(d):
    if _has(d, "pnpm-lock.yaml") or _has(util.root(d), "pnpm-lock.yaml"):
        return "pnpm"
    if _has(d, "yarn.lock") or _has(util.root(d), "yarn.lock"):
        return "yarn"
    if _has(d, "bun.lockb", "bun.lock"):
        return "bun"
    return "npm"


def py_runner(d):
    if _has(d, "uv.lock") and shutil.which("uv"):
        return "uv run "
    if _has(d, "poetry.lock") and shutil.which("poetry"):
        return "poetry run "
    return ""


def detect(cwd=None):
    """-> {'dir', 'stacks': [...], 'tests': [{'runner','cmd'}], 'checks': [{'name','cmd'}]}"""
    d = project_dir(cwd)
    info = {"dir": d, "stacks": [], "tests": [], "checks": []}
    if _has(d, "package.json"):
        pkg = _pkg(d)
        deps = dict(pkg.get("dependencies", {}), **pkg.get("devDependencies", {}))
        scripts = pkg.get("scripts", {})
        m = pm(d)
        info["stacks"].append("ts" if _has(d, "tsconfig.json") or "typescript" in deps else "js")
        st = scripts.get("test", "")
        if "vitest" in deps or "vitest" in st:
            b = _bin(d, "vitest")
            info["tests"].append({"runner": "vitest", "cmd": (b + " run") if b else m + " test"})
        elif "jest" in deps or "jest" in st:
            b = _bin(d, "jest")
            info["tests"].append({"runner": "jest", "cmd": b or m + " test"})
        elif st and "no test specified" not in st:
            info["tests"].append({"runner": "script", "cmd": m + " test"})
        for s in ("typecheck", "type-check", "tsc", "check-types"):
            if s in scripts:
                info["checks"].append({"name": s, "cmd": "%s run %s" % (m, s)})
                break
        else:
            if _has(d, "tsconfig.json") and _bin(d, "tsc"):
                info["checks"].append({"name": "tsc", "cmd": _bin(d, "tsc") + " --noEmit --pretty false"})
        if "lint" in scripts:
            info["checks"].append({"name": "lint", "cmd": "%s run lint" % m})
        elif _has(d, "eslint.config.js", "eslint.config.mjs", "eslint.config.cjs", "eslint.config.ts", ".eslintrc", ".eslintrc.js", ".eslintrc.json", ".eslintrc.cjs") and _bin(d, "eslint"):
            info["checks"].append({"name": "eslint", "cmd": _bin(d, "eslint") + " ."})
    if _has(d, "pyproject.toml", "setup.py", "setup.cfg", "pytest.ini", "tox.ini", "requirements.txt", "conftest.py") or \
            any(f.startswith("test_") and f.endswith(".py") for f in os.listdir(d)) or \
            (os.path.isdir(os.path.join(d, "tests")) and any(f.endswith(".py") for f in os.listdir(os.path.join(d, "tests")))):
        info["stacks"].append("py")
        pre = py_runner(d)
        info["tests"].append({"runner": "pytest", "cmd": pre + ("pytest" if pre else "python3 -m pytest")})
        cfg = ""
        for f in ("pyproject.toml", "setup.cfg"):
            try:
                with open(os.path.join(d, f)) as fh:
                    cfg += fh.read()
            except OSError:
                pass
        if shutil.which("ruff") or pre:
            # no project ruff config -> correctness rules only (ruff's own defaults now include style rules)
            own = "[tool.ruff" in cfg or _has(d, "ruff.toml", ".ruff.toml")
            info["checks"].append({"name": "ruff", "cmd": pre + "ruff check --output-format concise ." + ("" if own else " --select E4,E7,E9,F")})
        if ("[tool.mypy]" in cfg or "[mypy]" in cfg or _has(d, "mypy.ini")) and (shutil.which("mypy") or pre):
            info["checks"].append({"name": "mypy", "cmd": pre + "mypy ."})
        if (_has(d, "pyrightconfig.json") or "[tool.pyright]" in cfg) and (shutil.which("pyright") or pre):
            info["checks"].append({"name": "pyright", "cmd": pre + "pyright"})
    if _has(d, "go.mod"):
        info["stacks"].append("go")
        info["tests"].append({"runner": "go", "cmd": "go test ./..."})
        info["checks"].append({"name": "go vet", "cmd": "go vet ./..."})
        if _has(d, ".golangci.yml", ".golangci.yaml", ".golangci.toml") and shutil.which("golangci-lint"):
            info["checks"].append({"name": "golangci-lint", "cmd": "golangci-lint run"})
    if _has(d, "Cargo.toml"):
        info["stacks"].append("rust")
        info["tests"].append({"runner": "cargo", "cmd": "cargo test"})
        c, _ = util.sh(["cargo", "clippy", "--version"], timeout=10)
        info["checks"].append({"name": "clippy" if c == 0 else "cargo check",
                               "cmd": ("cargo clippy" if c == 0 else "cargo check") + " --all-targets --message-format short"})
    return info


def changed_files(cwd=None):
    c, out = util.sh(["git", "status", "--porcelain", "--untracked-files=all"], cwd=cwd, timeout=10)
    if c != 0:
        return []
    r = util.root(cwd)
    files = []
    for l in out.split("\n"):
        if len(l) > 3 and l[:2].strip() != "D":
            f = l[3:].split(" -> ")[-1].strip().strip('"')
            files.append(os.path.join(r, f))
    return files
