// Minimal dependency-free test harness.
//
//   #include "lob/check.hpp"
//   LOB_TEST(adds_an_order) { LOB_CHECK_EQ(1 + 1, 2); }
//   LOB_TEST_MAIN()
//
// Run all tests, or only those whose name contains argv[1]. Exit status is non-zero on any failure.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace lob::testing {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}
inline long& check_count() {
  static long n = 0;
  return n;
}
inline long& failure_count() {
  static long n = 0;
  return n;
}

inline bool register_test(const char* name, void (*fn)()) {
  registry().push_back({name, fn});
  return true;
}

template <class T>
std::string repr(const T& v) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else if constexpr (std::is_arithmetic_v<T>) {
    std::ostringstream out;
    out.precision(17);
    out << +v;
    return out.str();
  } else if constexpr (std::is_convertible_v<T, std::string>) {
    return std::string(v);
  } else {
    return "<value>";
  }
}

template <class A, class B>
void check_eq(const char* file, int line, const char* text_a, const char* text_b, const A& a,
              const B& b) {
  ++check_count();
  if (!(a == b)) {
    ++failure_count();
    std::fprintf(stderr, "  FAIL %s:%d: %s == %s (%s vs %s)\n", file, line, text_a, text_b,
                 repr(a).c_str(), repr(b).c_str());
  }
}

inline int run_all(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const TestCase& t : registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    const long failures_before = failure_count();
    t.fn();
    ++ran;
    std::printf("[%s] %s\n", failure_count() == failures_before ? " ok " : "FAIL", t.name);
  }
  std::printf("%d tests, %ld checks, %ld failures\n", ran, check_count(), failure_count());
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace lob::testing

#define LOB_TEST(name)                                                         \
  static void name();                                                          \
  static const bool lob_registered_##name = lob::testing::register_test(#name, &name); \
  static void name()

#define LOB_TEST_MAIN() \
  int main(int argc, char** argv) { return lob::testing::run_all(argc, argv); }

#define LOB_CHECK(cond)                                                              \
  do {                                                                               \
    ++lob::testing::check_count();                                                   \
    if (!(cond)) {                                                                   \
      ++lob::testing::failure_count();                                               \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                                \
  } while (0)

// The operands are function arguments, so any temporary in `a` or `b` (including one that a
// returned reference points into) lives until the check is done.
#define LOB_CHECK_EQ(a, b) lob::testing::check_eq(__FILE__, __LINE__, #a, #b, (a), (b))

#define LOB_CHECK_NEAR(a, b, tol)                                                    \
  do {                                                                               \
    ++lob::testing::check_count();                                                   \
    const double lob_va = static_cast<double>(a);                                    \
    const double lob_vb = static_cast<double>(b);                                    \
    if (!(std::fabs(lob_va - lob_vb) <= static_cast<double>(tol))) {                 \
      ++lob::testing::failure_count();                                               \
      std::fprintf(stderr, "  FAIL %s:%d: %s ~ %s within %s (%.17g vs %.17g)\n",     \
                   __FILE__, __LINE__, #a, #b, #tol, lob_va, lob_vb);                \
    }                                                                                \
  } while (0)
