// Usage: ut_tests [suite]   Runs every registered test, or only one suite. Exit code 1 if anything failed.
#include <cstdio>
#include <cstring>

#include "ut_test.h"

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failedCases = 0, checks = 0;
    for (const ut_test::Case& c : ut_test::registry()) {
        if (only && std::strcmp(only, c.suite) != 0) continue;
        ut_test::Current& cur = ut_test::current();
        cur.suite = c.suite;
        cur.name = c.name;
        cur.checks = 0;
        cur.failures = 0;
        c.fn();
        ++ran;
        checks += cur.checks;
        if (cur.failures) ++failedCases;
    }
    if (ran == 0) {
        std::printf("no tests in suite '%s'\n", only ? only : "(all)");
        return 1;
    }
    std::printf("%d cases, %d checks, %d failed\n", ran, checks, failedCases);
    return failedCases ? 1 : 0;
}
