// A tiny test runner: TEST(name) { CHECK(...); }, then runTests() from main.
#pragma once
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

struct TestCase { const char* name; std::function<void()> fn; };
inline std::vector<TestCase>& testCases() { static std::vector<TestCase> v; return v; }
inline int& testFailures() { static int n = 0; return n; }
struct TestReg { TestReg(const char* n, std::function<void()> f) { testCases().push_back({n, std::move(f)}); } };

#define TEST(name) static void test_##name(); static TestReg reg_##name(#name, test_##name); static void test_##name()
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); testFailures()++; } } while (0)
#define CHECK_EQ(a, b) do { auto va_ = (a); auto vb_ = (b); if (!(va_ == vb_)) { std::ostringstream o_; o_ << va_ << " != " << vb_; \
    printf("  FAIL %s:%d: %s == %s (%s)\n", __FILE__, __LINE__, #a, #b, o_.str().c_str()); testFailures()++; } } while (0)

inline int runTests() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // output that survives a crash, or a library closing stdout
    for (auto& t : testCases()) {
        int before = testFailures();
        printf("%s\n", t.name);
        t.fn();
        if (testFailures() == before) printf("  ok\n");
    }
    printf(testFailures() ? "\n%d check(s) failed\n" : "\nall passed\n", testFailures());
    return testFailures() ? 1 : 0;
}
