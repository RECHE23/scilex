// Smoke test: the test harness runs, and REAL (SciLex's only dependency) is
// reachable and working through the configured include path.
#include <string_view>

#include "framework.hpp"
#include "real/real.hpp"

using namespace std::string_view_literals;

TEST(harness_runs)
{
  EXPECT(true);
  EXPECT_EQ(1 + 1, 2);
}

TEST(real_dependency_is_available)
{
  const real::regex digits("[0-9]+");
  EXPECT(digits.search("abc123"));
  EXPECT_EQ(digits.search("abc123")[0], "123"sv);
}
