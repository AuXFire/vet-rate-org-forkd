"""Sample projects (one per stack). Scenarios start from these files and override some of them.

Each project has deliberate bugs so tests fail in realistic, varied ways:
  - an off-by-one that fails a whole parametrized family (same root cause → grouping)
  - a missing-key / null access raising inside library code (origin frame ≠ test frame)
"""

PY = {
    "pyproject.toml": "[tool.pytest.ini_options]\ntestpaths = ['tests']\n",
    "calc/__init__.py": "",
    "calc/core.py": (
        "def add(a, b):\n"
        "    return a + b + (1 if a > 5 else 0)\n"
        "\n"
        "\n"
        "def name(user):\n"
        "    return user['name'].strip()\n"
        "\n"
        "\n"
        "def ratio(a, b):\n"
        "    return a / b\n"
    ),
    "calc/fmt.py": (
        "from calc.core import ratio\n"
        "\n"
        "\n"
        "def pct(a, b):\n"
        "    return '%.1f%%' % (ratio(a, b) * 100)\n"
    ),
    "tests/test_core.py": (
        "import pytest\n"
        "from calc.core import add, name\n"
        "\n"
        "\n"
        "def test_add_small():\n"
        "    assert add(1, 2) == 3\n"
        "\n"
        "\n"
        "@pytest.mark.parametrize('a', [6, 7, 8, 9])\n"
        "def test_add_big(a):\n"
        "    assert add(a, 1) == a + 1\n"
        "\n"
        "\n"
        "def test_name():\n"
        "    assert name({'nm': ' x '}) == 'x'\n"
        "\n"
        "\n"
        "class TestStrings:\n"
        "    def test_compare(self):\n"
        "        assert {'a': 1, 'b': [1, 2, 3]} == {'a': 1, 'b': [1, 2, 4]}\n"
    ),
    "tests/test_fmt.py": (
        "from calc.fmt import pct\n"
        "\n"
        "\n"
        "def test_pct():\n"
        "    assert pct(1, 4) == '25.0%'\n"
        "\n"
        "\n"
        "def test_pct_zero():\n"
        "    assert pct(1, 0) == '0.0%'\n"
    ),
}

VITEST = {
    "package.json": '{"name":"vt","private":true,"type":"module","scripts":{"test":"vitest run"}}\n',
    "tsconfig.json": '{"compilerOptions":{"strict":true,"noEmit":true,"target":"es2022","module":"esnext","moduleResolution":"bundler","skipLibCheck":true},"include":["src"]}\n',
    "src/sum.ts": (
        "export const sum = (a: number, b: number): number => a + b + (a > 5 ? 1 : 0);\n"
        "export interface User { profile?: { name: string } }\n"
        "export function userName(u: User): string {\n"
        "  return u.profile!.name;\n"
        "}\n"
    ),
    "src/sum.test.ts": (
        "import { describe, test, expect } from 'vitest';\n"
        "import { sum, userName } from './sum';\n"
        "\n"
        "describe('sum', () => {\n"
        "  test('small', () => expect(sum(1, 2)).toBe(3));\n"
        "  for (const a of [6, 7, 8]) test(`big ${a}`, () => expect(sum(a, 1)).toBe(a + 1));\n"
        "});\n"
        "test('user', () => expect(userName({})).toBe('x'));\n"
        "test('objects', () => expect({ a: 1, list: [1, 2, 3] }).toEqual({ a: 1, list: [1, 2, 4] }));\n"
    ),
}

JEST = {
    "package.json": '{"name":"jt","private":true,"scripts":{"test":"jest"}}\n',
    "sum.js": (
        "const sum = (a, b) => a + b + (a > 5 ? 1 : 0);\n"
        "const user = (u) => u.profile.name;\n"
        "module.exports = { sum, user };\n"
    ),
    "sum.test.js": (
        "const { sum, user } = require('./sum');\n"
        "describe('sum', () => {\n"
        "  test('small', () => expect(sum(1, 2)).toBe(3));\n"
        "  for (const a of [6, 7, 8]) test(`big ${a}`, () => expect(sum(a, 1)).toBe(a + 1));\n"
        "});\n"
        "test('user', () => expect(user({})).toBe('x'));\n"
    ),
}

GO = {
    "go.mod": "module example.com/calc\n\ngo 1.21\n",
    "calc.go": (
        "package calc\n"
        "\n"
        "func Add(a, b int) int {\n"
        "\tif a > 5 {\n"
        "\t\treturn a + b + 1\n"
        "\t}\n"
        "\treturn a + b\n"
        "}\n"
        "\n"
        "func Name(m map[string]*string) string { return *m[\"name\"] }\n"
    ),
    "calc_test.go": (
        "package calc\n"
        "\n"
        "import \"testing\"\n"
        "\n"
        "func TestAdd(t *testing.T) {\n"
        "\tif Add(1, 2) != 3 {\n"
        "\t\tt.Fatal(\"small\")\n"
        "\t}\n"
        "}\n"
        "\n"
        "func TestAddBig(t *testing.T) {\n"
        "\tfor _, a := range []int{6, 7, 8} {\n"
        "\t\tt.Run(\"case\", func(t *testing.T) {\n"
        "\t\t\tif got := Add(a, 1); got != a+1 {\n"
        "\t\t\t\tt.Errorf(\"Add(%d,1) = %d; want %d\", a, got, a+1)\n"
        "\t\t\t}\n"
        "\t\t})\n"
        "\t}\n"
        "}\n"
    ),
    "util/util.go": "package util\n\nfunc Double(x int) int { return x * 2 }\n",
    "util/util_test.go": "package util\n\nimport \"testing\"\n\nfunc TestDouble(t *testing.T) {\n\tif Double(2) != 4 {\n\t\tt.Fatal(\"x\")\n\t}\n}\n",
}

RUST = {
    "Cargo.toml": '[package]\nname = "calc"\nversion = "0.1.0"\nedition = "2021"\n',
    "src/lib.rs": (
        "pub fn add(a: i32, b: i32) -> i32 {\n"
        "    if a > 5 { a + b + 1 } else { a + b }\n"
        "}\n"
        "\n"
        "pub fn first(v: &[i32]) -> i32 {\n"
        "    v[0]\n"
        "}\n"
        "\n"
        "#[cfg(test)]\n"
        "mod tests {\n"
        "    use super::*;\n"
        "    #[test]\n"
        "    fn small() { assert_eq!(add(1, 2), 3); }\n"
        "    #[test]\n"
        "    fn big() { assert_eq!(add(6, 1), 7); }\n"
        "    #[test]\n"
        "    fn big2() { assert_eq!(add(9, 1), 10, \"big2 case\"); }\n"
        "    #[test]\n"
        "    fn empty() { assert_eq!(first(&[]), 0); }\n"
        "}\n"
    ),
}

MAVEN = {
    "pom.xml": (
        '<project xmlns="http://maven.apache.org/POM/4.0.0"><modelVersion>4.0.0</modelVersion>\n'
        "<groupId>demo</groupId><artifactId>demo</artifactId><version>1.0</version>\n"
        "<properties><maven.compiler.source>17</maven.compiler.source><maven.compiler.target>17</maven.compiler.target>"
        "<project.build.sourceEncoding>UTF-8</project.build.sourceEncoding></properties>\n"
        "<dependencies><dependency><groupId>org.junit.jupiter</groupId><artifactId>junit-jupiter</artifactId>"
        "<version>5.10.2</version><scope>test</scope></dependency></dependencies>\n"
        "<build><plugins><plugin><groupId>org.apache.maven.plugins</groupId><artifactId>maven-surefire-plugin</artifactId>"
        "<version>3.2.5</version></plugin></plugins></build></project>\n"
    ),
    "src/main/java/demo/Calc.java": (
        "package demo;\n"
        "\n"
        "public class Calc {\n"
        "    public static int add(int a, int b) {\n"
        "        return a + b + (a > 5 ? 1 : 0);\n"
        "    }\n"
        "\n"
        "    public static String name(java.util.Map<String, String> m) {\n"
        "        return m.get(\"name\").trim();\n"
        "    }\n"
        "}\n"
    ),
    "src/test/java/demo/CalcTest.java": (
        "package demo;\n"
        "\n"
        "import org.junit.jupiter.api.Test;\n"
        "import org.junit.jupiter.params.ParameterizedTest;\n"
        "import org.junit.jupiter.params.provider.ValueSource;\n"
        "import static org.junit.jupiter.api.Assertions.*;\n"
        "\n"
        "class CalcTest {\n"
        "    @Test void small() { assertEquals(3, Calc.add(1, 2)); }\n"
        "    @ParameterizedTest @ValueSource(ints = {6, 7, 8}) void big(int a) { assertEquals(a + 1, Calc.add(a, 1)); }\n"
        "    @Test void name() { assertEquals(\"x\", Calc.name(java.util.Map.of(\"nm\", \"x\"))); }\n"
        "}\n"
    ),
}

RUBY = {
    "lib/calc.rb": (
        "module Calc\n"
        "  def self.add(a, b)\n"
        "    a + b + (a > 5 ? 1 : 0)\n"
        "  end\n"
        "\n"
        "  def self.name(h)\n"
        "    h.fetch(:name).strip\n"
        "  end\n"
        "end\n"
    ),
    "spec/calc_spec.rb": (
        "require_relative '../lib/calc'\n"
        "\n"
        "RSpec.describe Calc do\n"
        "  it('adds small') { expect(Calc.add(1, 2)).to eq(3) }\n"
        "  [6, 7, 8].each { |a| it(\"adds big #{a}\") { expect(Calc.add(a, 1)).to eq(a + 1) } }\n"
        "  it('names') { expect(Calc.name({ nm: 'x' })).to eq('x') }\n"
        "end\n"
    ),
    "test/calc_test.rb": (
        "require 'minitest/autorun'\n"
        "require_relative '../lib/calc'\n"
        "\n"
        "class CalcTest < Minitest::Test\n"
        "  def test_small\n"
        "    assert_equal 3, Calc.add(1, 2)\n"
        "  end\n"
        "\n"
        "  def test_big\n"
        "    assert_equal 7, Calc.add(6, 1)\n"
        "  end\n"
        "\n"
        "  def test_name\n"
        "    assert_equal 'x', Calc.name({ nm: 'x' })\n"
        "  end\n"
        "end\n"
    ),
}

C = {
    "Makefile": "CFLAGS=-Wall -Wextra -O0\nall: calc\ncalc: main.o calc.o\n\t$(CC) -o calc main.o calc.o\nclean:\n\trm -f *.o calc\n",
    "calc.h": "int add(int a, int b);\n",
    "calc.c": "#include \"calc.h\"\nint add(int a, int b) { return a + b; }\n",
    "main.c": "#include <stdio.h>\n#include \"calc.h\"\nint main(void) { printf(\"%d\\n\", add(1, 2)); return 0; }\n",
}

GRADLE = {
    "settings.gradle": "rootProject.name = 'demo'\n",
    "build.gradle": "plugins { id 'java' }\nrepositories { mavenCentral() }\ndependencies {\n  testImplementation 'org.junit.jupiter:junit-jupiter:5.10.2'\n  testRuntimeOnly 'org.junit.platform:junit-platform-launcher'\n}\ntest { useJUnitPlatform() }\n",
    "src/main/java/demo/Calc.java": MAVEN["src/main/java/demo/Calc.java"],
    "src/test/java/demo/CalcTest.java": MAVEN["src/test/java/demo/CalcTest.java"],
}

PROJECTS = {"gradle": GRADLE, "py": PY, "vitest": VITEST, "jest": JEST, "go": GO, "rust": RUST, "maven": MAVEN, "ruby": RUBY, "c": C}

# one-time dependency setup per project (results are shared across scenarios via symlink / cache)
SETUP = {
    "vitest": "npm i -D --no-audit --no-fund vitest@2.1.9 typescript@5.6.3 eslint@9.17.0 >/dev/null 2>&1",
    "jest": "npm i -D --no-audit --no-fund jest@29.7.0 >/dev/null 2>&1",
}
SHARED_DIRS = {"vitest": ["node_modules"], "jest": ["node_modules"]}
