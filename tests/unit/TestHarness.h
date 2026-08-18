#pragma once

// A deliberately tiny test harness. NFR-07 keeps third-party dependencies out
// of the core; a unit-test framework would be permitted, but at this size a
// few dozen lines cost less than a dependency.
//
//   TEST(name) { CHECK(cond); CHECK_EQ(a, b); }
//   int main() { return optiforge::test::runAll(); }

#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace optiforge::test {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline std::size_t& failureCount() {
  static std::size_t count = 0;
  return count;
}

inline void reportFailure(const char* file, int line, const std::string& what) {
  std::cerr << "  FAIL " << file << ':' << line << ": " << what << '\n';
  ++failureCount();
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

/// Renders a value for failure messages; falls back to a placeholder for types
/// that cannot be streamed.
template <typename T>
std::string show(const T& value) {
  std::ostringstream os;
  if constexpr (requires { os << value; }) {
    os << value;
  } else {
    os << "<unprintable>";
  }
  return os.str();
}

inline int runAll() {
  std::size_t failedTests = 0;
  for (const TestCase& test : registry()) {
    const std::size_t before = failureCount();
    test.fn();
    const bool failed = failureCount() != before;
    if (failed) {
      ++failedTests;
      std::cerr << "FAILED " << test.name << '\n';
    }
  }

  const std::size_t total = registry().size();
  if (failedTests == 0) {
    std::cout << "All " << total << " test(s) passed.\n";
    return 0;
  }
  std::cerr << failedTests << " of " << total << " test(s) failed.\n";
  return 1;
}

}  // namespace optiforge::test

#define OF_CONCAT_INNER(a, b) a##b
#define OF_CONCAT(a, b) OF_CONCAT_INNER(a, b)

#define TEST(name)                                                                   \
  static void OF_CONCAT(of_test_fn_, __LINE__)();                                    \
  static const ::optiforge::test::Registrar OF_CONCAT(of_test_reg_, __LINE__){       \
      name, &OF_CONCAT(of_test_fn_, __LINE__)};                                      \
  static void OF_CONCAT(of_test_fn_, __LINE__)()

// Variadic so braced initializers -- CHECK(Loc{0, 1, 1}.isValid()) -- are not
// split into several macro arguments by their internal commas.
#define CHECK(...)                                                                   \
  do {                                                                               \
    if (!(__VA_ARGS__)) {                                                            \
      ::optiforge::test::reportFailure(__FILE__, __LINE__,                           \
                                       "CHECK(" #__VA_ARGS__ ")");                   \
    }                                                                                \
  } while (false)

#define CHECK_EQ(a, b)                                                               \
  do {                                                                               \
    const auto& of_lhs = (a);                                                        \
    const auto& of_rhs = (b);                                                        \
    if (!(of_lhs == of_rhs)) {                                                       \
      ::optiforge::test::reportFailure(                                              \
          __FILE__, __LINE__,                                                        \
          std::string("CHECK_EQ(" #a ", " #b ")\n    lhs = ") +                      \
              ::optiforge::test::show(of_lhs) + "\n    rhs = " +                     \
              ::optiforge::test::show(of_rhs));                                      \
    }                                                                                \
  } while (false)
