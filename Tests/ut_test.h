// Minimal test harness. Failures print as "file:line: error: [suite.name] ..." so editors, ctest and lean
// all pick up the location.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ut_test {

struct Case {
    const char* suite;
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Current {
    const char* suite = "";
    const char* name = "";
    int checks = 0;
    int failures = 0;
};

inline Current& current() {
    static Current c;
    return c;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) { registry().push_back({suite, name, fn}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    Current& c = current();
    ++c.failures;
    std::printf("%s:%d: error: [%s.%s] %s\n", file, line, c.suite, c.name, what.c_str());
}

inline std::string show(double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}
inline std::string show(const std::string& v) { return "\"" + v + "\""; }
inline std::string show(const char* v) { return std::string("\"") + v + "\""; }
inline std::string show(bool v) { return v ? "true" : "false"; }
template <typename T>
std::string show(const T& v) {
    return show(static_cast<double>(v));
}

}  // namespace ut_test

#define UT_TEST(suite, name)                                                                   \
    static void ut_test_##suite##_##name();                                                    \
    static ut_test::Registrar ut_reg_##suite##_##name(#suite, #name, &ut_test_##suite##_##name); \
    static void ut_test_##suite##_##name()

#define UT_CHECK(expr)                                                                    \
    do {                                                                                  \
        ++ut_test::current().checks;                                                      \
        if (!(expr)) ut_test::fail(__FILE__, __LINE__, "CHECK(" #expr ") failed");        \
    } while (0)

#define UT_CHECK_EQ(a, b)                                                                              \
    do {                                                                                               \
        ++ut_test::current().checks;                                                                   \
        const auto ut_a = (a);                                                                         \
        const auto ut_b = (b);                                                                         \
        if (!(ut_a == ut_b))                                                                           \
            ut_test::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ") failed: " + ut_test::show(ut_a) + " vs " + ut_test::show(ut_b)); \
    } while (0)

#define UT_CHECK_NEAR(a, b, eps)                                                                       \
    do {                                                                                               \
        ++ut_test::current().checks;                                                                   \
        const double ut_a = static_cast<double>(a);                                                    \
        const double ut_b = static_cast<double>(b);                                                    \
        if (!(std::fabs(ut_a - ut_b) <= static_cast<double>(eps)))                                     \
            ut_test::fail(__FILE__, __LINE__, "CHECK_NEAR(" #a ", " #b ") failed: " + ut_test::show(ut_a) + " vs " + ut_test::show(ut_b)); \
    } while (0)
