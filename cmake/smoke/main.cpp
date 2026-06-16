// Downstream consumer smoke test: built by a separate CMake project that locates
// SciLex only through `find_package(scilex CONFIG)` against an installed copy.
// It also drives REAL (real::regex in the rules), so it proves the whole
// installed chain — scilex::scilex plus its transitive real::real — resolves
// from a single find_package.
#include <utility>
#include <vector>

#include "scilex/scilex.hpp"

int main()
{
  std::vector<scilex::rule> rules;
  rules.push_back({.kind = 0, .pattern = real::regex("[a-z]+"), .skip = false});
  rules.push_back({.kind = 1, .pattern = real::regex("[0-9]+"), .skip = false});
  const scilex::lexer lex {std::move(rules)};
  return lex.tokenize("ab12").size() == 2 ? 0 : 1;
}
