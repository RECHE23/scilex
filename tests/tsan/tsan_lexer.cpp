// ThreadSanitizer harness: one const scilex::lexer shared by several threads, each lexing its own text.
//
// The threads guide promises that a const lexer may be shared; this is what checks it. The shared state
// a scan can reach is REAL's: the per-regex DFA caches every thread leases from the regex's pool, the
// lazily built immutables, and the per-mode DFA the lexer built at construction. Each wave builds a FRESH
// lexer and releases all threads at once on a barrier, so the first scan of every thread is the one that
// fills those caches -- a staggered start would let the first thread fill them alone and hide the race.
//
// Grammars cover each path a scan takes:
//   json            every rule on the mode's DFA
//   xml             modes (push / pop), each on its own DFA
//   yaml            layout (indentation tokens) around a DFA mode
//   python-unicode  hybrid: rules the DFA cannot take run on Pike beside it (asserted below)
//
// Each thread's tokens are compared with a single-threaded reference: a wrong token stream is a failure
// even when TSan is silent.
//
// Injected-race proof: SCILEX_TSAN_INJECT_RACE=1 writes an unsynchronized counter from every thread in
// the barrier window; TSan must report it, which shows the harness can go red.
//
// Build & run: make tsan

#include <scilex/scilex.hpp>

#include "json.hpp"
#include "python.hpp"
#include "xml.hpp"
#include "yaml.hpp"

#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>
#include <thread>
#include <vector>

namespace {

  constexpr int k_threads        {8};
  constexpr int k_waves          {60};
  constexpr int k_scans          {4}; // per thread per wave: the first fills the caches, the rest read them warm
  int           g_inject_counter {0}; // written without synchronization only under SCILEX_TSAN_INJECT_RACE

  [[nodiscard]] bool inject_race_enabled()
  {
    // Read in main before any worker starts.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* e {std::getenv("SCILEX_TSAN_INJECT_RACE")};
    return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
  }

  struct grammar_case
  {
    const char *                   name;
    std::function<scilex::lexer()> make;
    std::string_view               text;
  };

  [[nodiscard]] bool same_tokens(const std::vector<scilex::token>& a,
                                 const std::vector<scilex::token>& b)
  {
    if (a.size() != b.size()) {
      return false;
    }
    for (std::size_t i {0}; i < a.size(); ++i) {
      if (a[i].kind != b[i].kind || a[i].lexeme != b[i].lexeme) {
        return false;
      }
    }
    return true;
  }

  // One wave: a fresh lexer, every thread's first scan at once. Returns the threads whose tokens
  // differed from the reference.
  int wave(const grammar_case&                c,
           const std::vector<scilex::token>&  reference,
           bool                               inject)
  {
    const scilex::lexer      lex   {c.make()};
    std::barrier             sync  {k_threads};
    std::atomic<int>         wrong {0};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(k_threads));
    for (int t {0}; t < k_threads; ++t) {
      threads.emplace_back([&, t] {
                             sync.arrive_and_wait();
                             if (inject) {
                               ++g_inject_counter; // the deliberate race
                               g_inject_counter += t;
                             }
                             for (int s {0}; s < k_scans; ++s) {
                               if (!same_tokens(lex.tokenize(c.text), reference)) {
                                 wrong.fetch_add(1, std::memory_order_relaxed);
                               }
                             }
                           });
    }
    for (std::thread& th : threads) {
      th.join();
    }
    return wrong.load(std::memory_order_relaxed);
  }
} // namespace

int main()
{
  const grammar_case cases[] {
    {.name = "json", .make = scilex::examples::json::make_lexer, .text = scilex::examples::json::sample},
    {.name = "xml", .make = scilex::examples::xml::make_lexer, .text = scilex::examples::xml::sample},
    {.name = "yaml", .make = scilex::examples::yaml::make_lexer, .text = scilex::examples::yaml::sample},
    {.name = "python-unicode", .make = scilex::examples::python::make_lexer_unicode,
     .text = scilex::examples::python::sample},
  };

  // The hybrid case must BE hybrid: a DFA in its default mode and rules on Pike beside it. Without that
  // the Pike-beside-DFA path goes unexercised and this case silently checks what json already does.
  {
    const scilex::lexer hybrid {scilex::examples::python::make_lexer_unicode()};
    if (hybrid.dfa_modes_active().empty() || hybrid.pike_rules("default").empty()) {
      std::printf("tsan_lexer: FAIL python-unicode is not hybrid (DFA modes %zu, Pike rules %zu)\n",
                  hybrid.dfa_modes_active().size(), hybrid.pike_rules("default").size());
      return 2;
    }
  }

  const bool inject {inject_race_enabled()};
  if (inject) {
    std::printf("tsan_lexer: SCILEX_TSAN_INJECT_RACE=1 -- expecting TSan to report a race\n");
  }

  int wrong {0};
  for (const grammar_case& c : cases) {
    const std::vector<scilex::token> reference {c.make().tokenize(c.text)};
    if (reference.empty()) {
      std::printf("tsan_lexer: FAIL %s produced no tokens\n", c.name);
      return 2;
    }
    for (int w {0}; w < k_waves; ++w) {
      wrong += wave(c, reference, inject);
    }
  }
  if (wrong != 0) {
    std::printf("tsan_lexer: FAIL %d scans returned tokens that differ from the single-threaded ones\n", wrong);
    return 1;
  }
  std::printf("tsan_lexer: OK (%d threads x %d waves x %d scans x %zu grammars)\n", k_threads, k_waves, k_scans,
              std::size(cases));
  if (inject) {
    std::printf("tsan_lexer: inject counter=%d (TSan should have reported; the harness proof is broken if not)\n",
                g_inject_counter);
    return 3;
  }
  return 0;
}
