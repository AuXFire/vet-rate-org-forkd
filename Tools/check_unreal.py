#!/usr/bin/env python3
"""Static checks on the Unreal C++ in Source/Undertow/Game.

This is not a compiler. It catches the mistakes that most often stop an Unreal build before it starts:
  - a reflected header whose "<File>.generated.h" include is missing, misnamed or not the last include
  - a UCLASS without GENERATED_BODY(), or without the module's _API export macro
  - a member function declared in a header with no definition in the matching .cpp (linker error)
  - an #include of a project file that does not exist
  - unbalanced braces or parentheses
Exit code 1 and one "file:line: error: ..." line per problem.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULE = os.path.join(ROOT, "Source", "Undertow")
GAME = os.path.join(MODULE, "Game")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def strip_strings(text):
    return re.sub(r'"(\\.|[^"\\\n])*"', '""', text)


def check_file(path, problems):
    rel = os.path.relpath(path, ROOT)
    raw = open(path).read()
    code = strip_comments(raw)
    bare = strip_strings(code)
    for open_ch, close_ch in ("{}", "()"):
        if bare.count(open_ch) != bare.count(close_ch):
            problems.append("%s:1: error: unbalanced %s%s (%d vs %d)" % (rel, open_ch, close_ch, bare.count(open_ch), bare.count(close_ch)))
    includes = [(i + 1, m.group(1)) for i, line in enumerate(code.split("\n")) for m in [re.match(r'\s*#include\s+"([^"]+)"', line)] if m]
    for line, inc in includes:
        if inc.startswith(("Game/", "Sim/")) or inc == "Undertow.h":
            if not os.path.exists(os.path.join(MODULE, inc)):
                problems.append("%s:%d: error: includes missing file %s" % (rel, line, inc))
    if not path.endswith(".h"):
        return
    base = os.path.basename(path)[:-2]
    reflected = re.search(r"^\s*(UCLASS|USTRUCT|UENUM|UINTERFACE)\b", code, re.M)
    generated = [(line, inc) for line, inc in includes if inc.endswith(".generated.h")]
    if reflected:
        if not generated:
            problems.append("%s:1: error: reflected type but no %s.generated.h include" % (rel, base))
        else:
            line, inc = generated[-1]
            if inc != base + ".generated.h":
                problems.append("%s:%d: error: generated include must be %s.generated.h, found %s" % (rel, line, base, inc))
            all_includes = [i + 1 for i, l in enumerate(code.split("\n")) if re.match(r"\s*#include\b", l)]
            if all_includes and all_includes[-1] != line:
                problems.append("%s:%d: error: %s must be the last #include" % (rel, line, inc))
    elif generated:
        problems.append("%s:%d: error: includes %s but declares no reflected type" % (rel, generated[0][0], generated[0][1]))

    cpp_path = path[:-2] + ".cpp"
    cpp = strip_comments(open(cpp_path).read()) if os.path.exists(cpp_path) else ""
    lines = code.split("\n")
    for m in re.finditer(r"^\s*UCLASS\b[^\n]*\n\s*class\s+(\w+_API\s+)?(\w+)\s*:\s*public\s+\w+[^{]*\{", code, re.M):
        cls, start = m.group(2), code[:m.end()].count("\n")
        if not m.group(1):
            problems.append("%s:%d: error: class %s has no UNDERTOW_API export macro" % (rel, start + 1, cls))
        depth, end = 1, start
        for i in range(start + 1, len(lines)):
            depth += strip_strings(lines[i]).count("{") - strip_strings(lines[i]).count("}")
            if depth <= 0:
                end = i
                break
        body = lines[start + 1:end]
        if not any("GENERATED_BODY()" in l for l in body):
            problems.append("%s:%d: error: class %s has no GENERATED_BODY()" % (rel, start + 1, cls))
        inner = 0
        for k, l in enumerate(body):
            s = l.strip()
            at_class_level = inner == 0
            inner += strip_strings(l).count("{") - strip_strings(l).count("}")
            if not at_class_level or not s.endswith(";") or "(" not in s or "{" in s:
                continue
            if re.match(r"^(UPROPERTY|UFUNCTION|GENERATED|DECLARE_|friend|using|typedef|static_assert)\b", s) or re.search(r"=\s*(0|default|delete)\s*;$", s):
                continue
            prev = next((b.strip() for b in reversed(body[:k]) if b.strip()), "")
            if "BlueprintImplementableEvent" in prev:
                continue                      # body is generated; Blueprint implements it
            name = re.match(r"^(?:virtual\s+|static\s+|explicit\s+)*(?:[\w:<>,*&\s]+?[\s*&]+)?(~?\w+)\s*\(", s)
            if not name or name.group(1) in ("TObjectPtr", "TArray", "TMap", "TSubclassOf"):
                continue
            if not re.search(r"\b%s::%s\s*\(" % (re.escape(cls), re.escape(name.group(1))), cpp):
                problems.append("%s:%d: error: %s::%s is declared but not defined in %s.cpp" % (rel, start + 2 + k, cls, name.group(1), base))


def main():
    problems = []
    files = sorted(os.path.join(d, f) for d, _, fs in os.walk(MODULE) for f in fs if f.endswith((".h", ".cpp")) and os.sep + "Sim" + os.sep not in os.path.join(d, f))
    for f in files:
        check_file(f, problems)
    for p in problems:
        print(p)
    print("check_unreal: %d files, %d problems" % (len(files), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
