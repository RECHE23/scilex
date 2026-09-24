// The README's C++ quickstart, compiled and run by `make example`, which also checks that the README
// shows the marked region verbatim — so the code a reader copies is the code that is tested.
#include <cstdio>
#include <vector>

#include <scilex/scilex.hpp>

int main()
{
  // [quickstart]
  std::vector<scilex::rule> rules {
    {.kind = 0, .pattern = real::regex(R"(\s+)"), .skip = true}, // whitespace, skipped
    {.kind = 1, .pattern = real::regex("if")},                   // keyword: listed before the identifier
    {.kind = 2, .pattern = real::regex("[a-z_][a-z0-9_]*")},     // identifier
    {.kind = 3, .pattern = real::regex("[0-9]+")},               // number
    {.kind = 4, .pattern = real::regex(R"([-+*/=])")},           // operator
  };
  const scilex::lexer lexer {std::move(rules)};

  // Lazy: one token per step (tokenize() returns them all at once).
  for (const scilex::token& tok : lexer.scan("if x + 42")) {
    std::printf("%d %.*s\n", tok.kind, static_cast<int>(tok.lexeme.size()), tok.lexeme.data());
  }
  // [/quickstart]
  return 0;
}
