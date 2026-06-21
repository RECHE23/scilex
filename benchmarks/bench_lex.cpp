/*!
 * \file bench_lex.cpp
 * \brief Per-grammar C++ throughput baseline for the SciLex engine (MB/s).
 *
 * Complements \c benchmarks/bench.py (which times the Python binding against
 * \c re): this measures the pure C++ lexer directly, on each of the nine
 * example grammars, over realistic steady-state inputs built by scaling the
 * grammar's own sample. It reports MB/s for two paths:
 *
 *   - \c tokenize() — eager, materializes the whole token vector (O(n) memory);
 *   - \c scan()     — lazy, one token at a time (O(1) memory) — the path a
 *                     parser actually consumes.
 *
 * A final section contrasts the modal Python grammar with a mono-mode baseline
 * (the same code rules with the f-string modes stripped, so f-strings lex as a
 * name + plain string) on the same sample — isolating the mode-stack overhead.
 *
 * Methodology (no measurement artifact): each input is scaled to a steady-state
 * size, the lexer is built once, the timed region is warmed up, and the
 * **minimum** wall time over N repetitions is taken (the cleanest throughput
 * estimate — least exposed to scheduler jitter). Built at -O2. A volatile sink
 * consumes every result so nothing is optimized away. Informational only — never
 * gated; see `make bench-lex` / `make bench`. Sizes are KiB (1024 B), throughput
 * is MB/s (10^6 B/s).
 */
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "cpp.hpp"
#include "css.hpp"
#include "json.hpp"
#include "lisp.hpp"
#include "math.hpp"
#include "python.hpp"
#include "sql.hpp"
#include "xml.hpp"
#include "yaml.hpp"

namespace {

  using clock_type = std::chrono::steady_clock;

  constexpr std::size_t target_bytes {256 * 1024}; //!< Steady-state input size.
  constexpr int         warmup       {3};          //!< Untimed warmup passes.
  constexpr int         reps         {9};          //!< Timed passes; min is taken.

  volatile std::size_t g_sink        {0};          //!< Consumes results — defeats dead-code elimination.

  //! \brief Repeats \p sample (newline-separated) until at least \p target bytes.
  std::string scale(std::string_view sample,
                    std::size_t      target)
  {
    std::string out;
    out.reserve(target + sample.size() + 1);
    while (out.size() < target) {
      out.append(sample);
      out.push_back('\n'); // separate copies so no token merges across the join
    }
    return out;
  }

  //! \brief Minimum wall-seconds of \p run over \ref reps passes, after \ref warmup.
  template <class Run>
  double min_seconds(Run&& run)
  {
    for (int i {0}; i < warmup; ++i) {
      run();
    }
    double best {std::numeric_limits<double>::infinity()};
    for (int i {0}; i < reps; ++i) {
      const auto before {clock_type::now()};
      run();
      const auto after  {clock_type::now()};
      best = std::min(best, std::chrono::duration<double>(after - before).count());
    }
    return best;
  }

  //! \brief Throughput in MB/s (10^6 bytes per second).
  double mbps(std::size_t bytes,
              double      seconds)
  {
    return (static_cast<double>(bytes) / 1e6) / seconds;
  }

  //! \brief One grammar's measured row.
  struct row
  {
    const char* name;
    std::size_t bytes;
    std::size_t tokens;
    double      eager_mbps;
    double      lazy_mbps;
  };

  //! \brief Measures eager and lazy throughput of \p make's lexer on \p sample.
  row measure(const char     * name,
              scilex::lexer (* make)(),
              std::string_view sample)
  {
    const std::string   source {scale(sample, target_bytes)};
    const scilex::lexer lex    {make()};
    const std::size_t   tokens {lex.tokenize(source).size()};

    const double eager         {min_seconds([&] {
                                              g_sink += lex.tokenize(source).size();
                                            })};
    const double lazy          {min_seconds([&] {
                                              std::size_t consumed {0};
                                              for (const scilex::token& tok : lex.scan(source)) {
                                                consumed += tok.lexeme.size();
                                              }
                                              g_sink += consumed;
                                            })};
    return {name, source.size(), tokens, mbps(source.size(), eager), mbps(source.size(), lazy)};
  }
} // namespace

int main()
{
  std::printf("SciLex C++ engine throughput — per grammar (min-of-%d, %d warmup, -O2)\n", reps, warmup);
  std::printf("steady-state input: each grammar's sample scaled to >= %zu KiB\n\n", target_bytes / 1024);
  std::printf("  %-8s %8s %9s %13s %13s\n", "grammar", "KiB", "tokens", "eager MB/s", "lazy MB/s");
  std::printf("  %-8s %8s %9s %13s %13s\n", "-------", "---", "------", "----------", "---------");

  const row rows[] {
    measure("json", &scilex::examples::json::make_lexer, scilex::examples::json::sample),
    measure("python", &scilex::examples::python::make_lexer, scilex::examples::python::sample),
    measure("cpp", &scilex::examples::cpp::make_lexer, scilex::examples::cpp::sample),
    measure("sql", &scilex::examples::sql::make_lexer, scilex::examples::sql::sample),
    measure("css", &scilex::examples::css::make_lexer, scilex::examples::css::sample),
    measure("lisp", &scilex::examples::lisp::make_lexer, scilex::examples::lisp::sample),
    measure("math", &scilex::examples::math::make_lexer, scilex::examples::math::sample),
    measure("xml", &scilex::examples::xml::make_lexer, scilex::examples::xml::sample),
    measure("yaml", &scilex::examples::yaml::make_lexer, scilex::examples::yaml::sample),
  };
  for (const row& entry : rows) {
    std::printf("  %-8s %8zu %9zu %13.2f %13.2f\n",
                entry.name, entry.bytes / 1024, entry.tokens, entry.eager_mbps, entry.lazy_mbps);
  }

  // Linearity: one grammar over growing sizes. Flat MB/s == time scales with the
  // input == the linear, ReDoS-safe guarantee, shown directly in C++.
  std::printf("\nlinearity — cpp grammar at growing sizes (flat MB/s => linear time):\n");
  std::printf("  %8s %13s\n", "KiB", "eager MB/s");
  const scilex::lexer clex {scilex::examples::cpp::make_lexer()};
  for (const std::size_t kib : {std::size_t {64}, std::size_t {128}, std::size_t {256}, std::size_t {512}}) {
    const std::string source {scale(scilex::examples::cpp::sample, kib * 1024)};
    const double      best   {min_seconds([&] {
                                            g_sink += clex.tokenize(source).size();
                                          })};
    std::printf("  %8zu %13.2f\n", source.size() / 1024, mbps(source.size(), best));
  }

  // Mode overhead: the modal Python grammar vs a mono-mode baseline on the SAME
  // sample. The baseline drops the f-string mode rules (so f"…" lexes as a NAME +
  // plain string) and clears in_mode/action — the delta is the mode-stack cost.
  namespace py = scilex::examples::python;
  std::printf("\nmode overhead — python on its sample (modal vs a mono-mode baseline):\n");
  std::printf("  %-10s %8s %9s %13s\n", "variant", "KiB", "tokens", "eager MB/s");
  const std::string pysrc {scale(py::sample, target_bytes)};
  {
    const scilex::lexer modal {py::make_lexer()};
    const std::size_t   toks  {modal.tokenize(pysrc).size()};
    const double        secs  {min_seconds([&] {
                                             g_sink += modal.tokenize(pysrc).size();
                                           })};
    std::printf("  %-10s %8zu %9zu %13.2f\n", "modal", pysrc.size() / 1024, toks, mbps(pysrc.size(), secs));
  }
  {
    std::vector<scilex::rule> mono;
    for (scilex::rule& candidate : py::make_rules()) {
      const int kind {candidate.kind};
      if (kind == py::fstr_open || kind == py::fstr_text || kind == py::fstr_close
          || kind == py::interp_open || kind == py::interp_close) {
        continue; // drop the f-string mode rules (f-strings become NAME + string)
      }
      candidate.in_mode.clear();
      candidate.action.reset();
      mono.push_back(std::move(candidate));
    }
    const scilex::lexer base {std::move(mono)};
    const std::size_t   toks {base.tokenize(pysrc).size()};
    const double        secs {min_seconds([&] {
                                            g_sink += base.tokenize(pysrc).size();
                                          })};
    std::printf("  %-10s %8zu %9zu %13.2f\n", "mono-mode", pysrc.size() / 1024, toks, mbps(pysrc.size(), secs));
  }

  (void)g_sink;
  return 0;
}
