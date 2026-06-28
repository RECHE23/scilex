# Thin orchestrator over CMake (build/test/sanitize/coverage) and the QA tools
# (clang-tidy, doxygen, uncrustify). CMake owns all compilation policy
# (CMakeLists.txt); this file only wires the frequent commands.
#
# Override the compiler with CXX on the command line, e.g.
#   make test CXX=g++-14
# Switching compilers reuses a cached build dir, so run `make clean` first.
#
# SciLex depends on REAL's headers. REAL_INCLUDE points at them.
# When cloning the repo, the common name is "real-regex" (to match the GitHub repo).
# The default below is the maintainer's local layout. Override with
#   make ... REAL_INCLUDE=/path/to/real-regex/include
# or use FETCH=1 for CI. Use `python -c "import real; print(real.get_include())"`
# when REAL is installed.

CMAKE  ?= cmake
CTEST  ?= ctest
BUILD  := build
PYTHON ?= python3
# SciForge owns the shared, dev-only benchmark substrate (sciforge.bench), in its python/ dir.
# Same sibling default as SCIFORGE_INCLUDE/_LINT; never a build/runtime dep, never shipped.
SCIFORGE_PYTHON ?= ../sciforge/python
# Append the SciForge sibling so the benchmark can `import sciforge.bench` (compare/verdict/stats).
PYRUN  := PYTHONPATH=$(CURDIR)/python:$(abspath $(SCIFORGE_PYTHON)) $(PYTHON)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
# Parallelism: detected core count (override with JOBS=N).
JOBS   ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

REAL_INCLUDE ?= $(CURDIR)/../real-regex/include  # common names: real-regex, real, real-regex

ifeq ($(origin CXX),command line)
CMAKE_CXX := -DCMAKE_CXX_COMPILER=$(CXX)
endif

CMAKE_REAL   := -DSCILEX_REAL_INCLUDE=$(REAL_INCLUDE)
# FETCH=1 fetches REAL via FetchContent instead of the sibling dir (for CI /
# reproducible builds); the fetched include overrides the path above.
ifeq ($(FETCH),1)
CMAKE_REAL += -DSCILEX_FETCH_DEPS=ON
endif
CXXSTD       := -std=c++20
# REAL is a dependency: include it as a system header so the linters analyze
# SciLex's own code only (REAL passes its own gates).
INCLUDES     := -Iinclude -isystem $(REAL_INCLUDE)
# The test harness (framework.hpp) is owned by SciForge; the test TUs include
# it as <sciforge/test/framework.hpp>. clang-tidy (make lint) needs that path too.
SCIFORGE_INCLUDE ?= ../sciforge/include
# SciForge also owns the shared lint config (the MISRA base + uncrustify.cfg), in
# its lint/ dir. Same sibling default; CI checks SciForge out alongside as ../sciforge.
SCIFORGE_LINT ?= ../sciforge/lint
FORMAT_FILES := $(shell find include tests examples fuzz benchmarks cli -name '*.hpp' -o -name '*.cpp')

.PHONY: all build test sanitize coverage coverage-build coverage-html \
        lint misra doc doc-no-coverage format format-check full-local-gate \
        python python-test bench bench-lex cli example fuzz-check fuzz version-check install uninstall install-cli uninstall-cli release clean help

.DEFAULT_GOAL := help

help:
	@echo "SciLex — build orchestrator (CMake + QA tools)"
	@echo ""
	@echo "  make build      Configure and build the test binary (CMake)"
	@echo "  make test       Build and run the test suite (ctest)"
	@echo "  make sanitize   Build and run the tests under ASan + UBSan"
	@echo "  make coverage   Line-coverage text summary + HTML report"
	@echo "  make lint       clang-tidy over the test sources"
	@echo "  make misra      MISRA C++:2023-oriented analysis"
	@echo "  make doc        Generate API reference (Doxygen) with embedded coverage"
	@echo "  make doc-no-coverage  Generate API reference without coverage report"
	@echo "  make format     Uncrustify, in place"
	@echo "  make format-check  Uncrustify, dry-run, exits non-zero on diff"
	@echo "  make python     Build the Python extension in place (abi3)"
	@echo "  make python-test  Run the Python binding test suite"
	@echo "  make bench      C++ per-grammar throughput + Python re comparison (informational)"
	@echo "  make bench-lex  C++ per-grammar engine throughput only, MB/s (informational)"
	@echo "  make cli        Build the scilex command-line lexer (cli/scilex.cpp)"
	@echo "  make example    Build the CLI and run every example self-check (gate)"
	@echo "  make fuzz-check Deterministic lexer-oracle gate (property invariants)"
	@echo "  make fuzz       libFuzzer robustness fuzzing of the lexer (Clang; FUZZ_TIME=secs)"
	@echo "  make version-check  Assert pyproject = __init__ = CMake-derived version"
	@echo "  make full-local-gate  Every gate in one command (the macOS gate of record)"
	@echo "  make install    Install the Python package (pip)"
	@echo "  make install-cli  Install the scilex CLI to PREFIX/bin (opt-in; DESTDIR-aware)"
	@echo "  make uninstall  Uninstall the Python package (pip)"
	@echo "  make release    Cut a calendar-versioned release (tag + push)"
	@echo "  make clean      Remove build artifacts"
	@echo ""
	@echo "  Override the compiler:   make test CXX=g++-14"
	@echo "  Override REAL's headers: make test REAL_INCLUDE=/path/to/real/include"

all: build

# --- build / test (delegated to CMake) ------------------------------------

build:
	$(CMAKE) -S . -B $(BUILD) $(CMAKE_CXX) $(CMAKE_REAL) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)

test: build
	$(CTEST) --test-dir $(BUILD) --output-on-failure

sanitize:
	$(CMAKE) -S . -B $(BUILD)/sanitize $(CMAKE_CXX) $(CMAKE_REAL) -DSCILEX_SANITIZE=ON
	$(CMAKE) --build $(BUILD)/sanitize --parallel $(JOBS)
	$(CTEST) --test-dir $(BUILD)/sanitize --output-on-failure

# Coverage uses LLVM source-based instrumentation, so it pins a Clang
# toolchain end to end. On macOS the Apple toolchain is required (Homebrew
# clang links a profile runtime its llvm-profdata cannot read).
ifeq ($(shell uname -s),Darwin)
COV_CXX  ?= /usr/bin/clang++
PROFDATA ?= xcrun llvm-profdata
LLVM_COV ?= xcrun llvm-cov
else
COV_CXX  ?= clang++
PROFDATA ?= llvm-profdata
LLVM_COV ?= llvm-cov
endif
COV_DIR  := $(BUILD)/coverage

coverage-build:
	$(CMAKE) -S . -B $(COV_DIR) $(CMAKE_REAL) -DSCILEX_COVERAGE=ON -DCMAKE_CXX_COMPILER=$(COV_CXX)
	$(CMAKE) --build $(COV_DIR) --parallel $(JOBS)
	LLVM_PROFILE_FILE=$(COV_DIR)/tests.profraw $(COV_DIR)/scilex_tests_bin
	$(PROFDATA) merge -sparse $(COV_DIR)/tests.profraw -o $(COV_DIR)/tests.profdata

coverage: coverage-build
	$(LLVM_COV) report $(COV_DIR)/scilex_tests_bin -instr-profile=$(COV_DIR)/tests.profdata \
	    $(CURDIR)/include
	$(LLVM_COV) show $(COV_DIR)/scilex_tests_bin -instr-profile=$(COV_DIR)/tests.profdata \
	    -format=html -output-dir=$(COV_DIR)/html -show-line-counts-or-regions
	@grep -q "SciLex dark-coverage theme" $(COV_DIR)/html/style.css 2>/dev/null || \
	    cat docs/coverage-style.css >> $(COV_DIR)/html/style.css
	@echo "HTML coverage report: $(COV_DIR)/html/index.html"

coverage-html:
	@mkdir -p $(COV_DIR)
	@$(MAKE) --silent coverage-build > $(COV_DIR)/build.log 2>&1 || (cat $(COV_DIR)/build.log; exit 1)
	@$(LLVM_COV) show $(COV_DIR)/scilex_tests_bin -instr-profile=$(COV_DIR)/tests.profdata \
	    -format=html -output-dir=$(COV_DIR)/html -show-line-counts-or-regions
	@grep -q "SciLex dark-coverage theme" $(COV_DIR)/html/style.css 2>/dev/null || \
	    cat docs/coverage-style.css >> $(COV_DIR)/html/style.css
	@echo "HTML coverage report: $(COV_DIR)/html/index.html"

# --- QA tools (wrappers; no compilation policy here) ----------------------

lint:
	@ls tests/*.cpp | xargs -P $(JOBS) -I{} clang-tidy {} -- $(CXXSTD) $(INCLUDES) -I$(SCIFORGE_INCLUDE)

misra:
	mkdir -p $(BUILD)
	printf '#include <scilex/scilex.hpp>\nint main(){ try { const scilex::lexer l({{0, real::regex("a")}}); return l.tokenize("a").size() == 1 ? 0 : 1; } catch (...) { return 2; } }\n' > $(BUILD)/misra_tu.cpp
	clang-tidy --config-file=$(SCIFORGE_LINT)/clang-tidy-misra \
	    --header-filter='include/scilex/.*' \
	    $(BUILD)/misra_tu.cpp -- $(CXXSTD) $(INCLUDES)

doc: coverage-html
	mkdir -p $(BUILD)/doc
	doxygen Doxyfile
	@rm -rf $(BUILD)/doc/html/coverage
	@cp -R $(COV_DIR)/html $(BUILD)/doc/html/coverage
	@echo "API reference: $(BUILD)/doc/html/index.html"

doc-no-coverage:
	mkdir -p $(BUILD)/doc
	doxygen Doxyfile
	@echo "API reference: $(BUILD)/doc/html/index.html"

format:
	uncrustify -c $(SCIFORGE_LINT)/uncrustify.cfg --replace --no-backup $(FORMAT_FILES)

# Python binding: an abi3 CPython extension (Limited API) over the C++ lexer.
# Build against $(REAL_INCLUDE) (the sibling by default) via SCILEX_REAL_INCLUDE, so
# local co-development uses the current REAL headers — not a stale installed package.
# A wheel/pip build (no env set) resolves REAL through setup.py instead.
python:
	SCIFORGE_INCLUDE=$(SCIFORGE_INCLUDE) SCILEX_REAL_INCLUDE=$(REAL_INCLUDE) $(PYTHON) setup.py -q build_ext --inplace

python-test: python
	$(PYRUN) -m unittest discover -s python/tests

# Wall-time micro-benchmarks vs Python's re (informational; never gated). See BENCHMARKS.md.
# C++ per-grammar engine throughput (MB/s) on the example grammars, scaled to
# steady state. Standalone (no Python build); informational, never gated.
bench-lex:
	@mkdir -p $(BUILD)/benchmarks
	c++ $(CXXSTD) -O2 -Wall -Wextra -Wpedantic -Werror $(INCLUDES) -Iexamples benchmarks/bench_lex.cpp -o $(BUILD)/benchmarks/bench_lex
	@$(BUILD)/benchmarks/bench_lex

# The full benchmark picture: the C++ engine throughput first, then the Python
# binding versus re. Informational only — never part of full-local-gate.
bench: bench-lex python
	$(PYRUN) benchmarks/bench.py

# Builds the scilex CLI (cli/scilex.cpp). The example grammars live in reusable
# headers (examples/<lang>.hpp, namespace scilex::examples::<lang>) so the CLI,
# bench, and fuzzer all share them; the CLI reuses that same registry for its
# built-in --example grammars, and parses user .lex files for everything else.
# Compiled under -Werror.
cli:
	@mkdir -p $(BUILD)/bin
	c++ $(CXXSTD) -O2 -Wall -Wextra -Wpedantic -Werror $(INCLUDES) -Iexamples cli/scilex.cpp -o $(BUILD)/bin/scilex
	@echo "cli: built $(BUILD)/bin/scilex"

# The example gate: build the CLI, then run every grammar's self-check through
# `scilex --check`. A failing self-check fails the gate.
example: cli
	@$(BUILD)/bin/scilex --check
	@echo "examples: all self-checks pass"

# Deterministic lexer-oracle gate (fuzz/reference.hpp): runs every property invariant
# (munch equivalence vs an independent reference scanner, coverage, lazy==eager, layout
# balance, positioned errors, determinism) over the example grammars x a fixed adversarial
# input set, compiled -Werror. The continuous libFuzzer explorer is `make fuzz`.
fuzz-check:
	@mkdir -p $(BUILD)/fuzz
	c++ $(CXXSTD) -O2 -Wall -Wextra -Wpedantic -Werror $(INCLUDES) -Iexamples -Ifuzz fuzz/oracle_check.cpp -o $(BUILD)/fuzz/oracle_check
	@$(BUILD)/fuzz/oracle_check

# Continuous, coverage-guided fuzzing carrying the property oracle (fuzz/reference.hpp):
# Clang + libFuzzer + ASan/UBSan. NOT part of the gate (it runs unbounded) — the bounded,
# deterministic gate is `make fuzz-check`. FUZZ_TIME bounds a local run; the corpus lives
# under build/ (not versioned), seeded from the example grammars and tests as representative
# byte sequences. A wrong tokenization aborts with the grammar and the violated invariant.
FUZZ_TIME ?= 30
fuzz:
	@mkdir -p $(BUILD)/fuzz/corpus
	@cp examples/*.hpp tests/*.cpp $(BUILD)/fuzz/corpus/ 2>/dev/null || true
	clang++ $(CXXSTD) -O1 -g -fsanitize=fuzzer,address,undefined $(INCLUDES) -Iexamples -Ifuzz fuzz/fuzz_lexer.cpp -o $(BUILD)/fuzz/fuzz_lexer
	$(BUILD)/fuzz/fuzz_lexer -max_total_time=$(FUZZ_TIME) -timeout=10 -max_len=8192 $(BUILD)/fuzz/corpus

# Version-consistency gate: pyproject.toml is the single source of truth — `make
# release` bumps __init__.py from it, CMakeLists.txt derives it. Asserts the three
# agree and that CMake still DERIVES (no hardcoded literal that could drift) — the
# invariant the CMake 2026.6.1-vs-.3 drift violated.
version-check:
	@py=$$(sed -nE 's/^version = "([0-9][0-9.]*)"/\1/p' pyproject.toml); \
	 ini=$$(sed -nE 's/^__version__ = "([0-9][0-9.]*)"/\1/p' python/scilex/__init__.py); \
	 if [ -z "$$py" ]; then echo "version-check: no version found in pyproject.toml"; exit 1; fi; \
	 if [ "$$py" != "$$ini" ]; then echo "version-check: DRIFT pyproject=$$py vs __init__=$$ini"; exit 1; fi; \
	 if ! grep -q 'file(READ.*pyproject\.toml' CMakeLists.txt; then echo "version-check: CMakeLists.txt must derive its version from pyproject.toml"; exit 1; fi; \
	 if grep -qE '^project\([A-Za-z_]+ VERSION [0-9]' CMakeLists.txt; then echo "version-check: CMakeLists.txt has a hardcoded VERSION (must derive from pyproject.toml)"; exit 1; fi; \
	 echo "version-check: $$py (pyproject = __init__ = CMake-derived)"

# The complete local quality gate in one command — the canonical pre-push check
# and, since CI no longer runs macOS (see .github/workflows/ci.yml), the macOS gate
# of record. Runs every check this machine owns and fails on any issue, including a
# lint warning or any coverage dimension below 100%. If an incremental coverage build
# is ever suspect, `rm -rf $(COV_DIR)` first to force a clean re-measure.
full-local-gate:
	@$(MAKE) format-check
	@$(MAKE) version-check
	@$(MAKE) test
	@$(MAKE) test CXX=g++-14 BUILD=$(BUILD)/gcc
	@$(MAKE) sanitize
	@$(MAKE) misra
	@$(MAKE) doc-no-coverage
	@$(MAKE) python-test
	@$(MAKE) example
	@$(MAKE) fuzz-check
	@$(MAKE) lint | tee $(BUILD)/lint.log; ! grep -qE 'warning:|error:' $(BUILD)/lint.log
	@$(MAKE) coverage | tee $(BUILD)/coverage.log
	# Gate on a flag, not a bare exit 1 in the rule: that exit is overridden by END's exit,
	# so the gate silently accepted coverage below 100% 4D before this fix.
	@awk '/^TOTAL/{seen=1; if (gsub(/100\.00%/, "&") != 4) bad=1} END{exit (seen && !bad) ? 0 : 1}' $(BUILD)/coverage.log \
	  || { echo "full-local-gate: coverage is below 100% on some dimension — see above"; exit 1; }
	@echo "full-local-gate: ALL gates green (clang + g++-14, sanitize, MISRA, lint, doc, python, example, fuzz-check, version-check, 100% coverage)"

format-check:
	uncrustify -c $(SCIFORGE_LINT)/uncrustify.cfg --check $(FORMAT_FILES)

install:
	$(PYTHON) -m pip install .

uninstall:
	$(PYTHON) -m pip uninstall -y scilex

# Install the scilex CLI binary — opt-in and separate from the pip package, so a
# plain `make install` never needs write access to a system prefix. Override the
# location with PREFIX= or BINDIR=; DESTDIR is honoured for staged installs.
install-cli: cli
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BUILD)/bin/scilex $(DESTDIR)$(BINDIR)/scilex
	@echo "installed: $(DESTDIR)$(BINDIR)/scilex (override location with PREFIX= or BINDIR=)"

uninstall-cli:
	rm -f $(DESTDIR)$(BINDIR)/scilex

# Cuts a calendar-versioned release: computes YYYY.M.PATCH with the patch reset
# each month (first release of a month is .0; PEP 440 drops leading zeros, so
# 2026.6.1, never 2026.06.001), bumps pyproject.toml + __init__.py (CMakeLists.txt
# derives its version from pyproject.toml, so it follows automatically), commits, tags and
# pushes from a clean main. Pushing the tag IS the release. PyPI publishing is a
# separate, per-package opt-in (add a release.yml workflow + a PyPI Trusted
# Publisher); without it, this just creates a versioned git tag.
release:
	@test "$$(git symbolic-ref --short HEAD)" = main || { echo "release from main only"; exit 1; }
	@test -z "$$(git status --porcelain)" || { echo "working tree not clean"; exit 1; }
	@git fetch --tags --quiet origin
	@year=$$(date -u +%Y); month=$$(date -u +%m | sed 's/^0//'); \
	 patch=$$(git tag -l "v$$year.$$month.*" | wc -l | tr -d ' '); \
	 version="$$year.$$month.$$patch"; \
	 echo "Releasing v$$version"; \
	 sed -i.bak -E "s/^version = \".*\"/version = \"$$version\"/" pyproject.toml && rm -f pyproject.toml.bak; \
	 sed -i.bak -E "s/^__version__ = \".*\"/__version__ = \"$$version\"/" python/scilex/__init__.py && rm -f python/scilex/__init__.py.bak; \
	 git add pyproject.toml python/scilex/__init__.py; \
	 git commit -m "release: v$$version"; \
	 git tag "v$$version"; \
	 git push origin HEAD "v$$version"

clean:
	rm -rf $(BUILD)
