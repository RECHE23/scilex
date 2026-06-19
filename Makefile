# Thin orchestrator over CMake (build/test/sanitize/coverage) and the QA tools
# (clang-tidy, doxygen, uncrustify). CMake owns all compilation policy
# (CMakeLists.txt); this file only wires the frequent commands.
#
# Override the compiler with CXX on the command line, e.g.
#   make test CXX=g++-14
# Switching compilers reuses a cached build dir, so run `make clean` first.
#
# SciLex depends on REAL's headers. REAL_INCLUDE points at them (default: a
# sibling ../real-v1/include); override on the command line if REAL lives
# elsewhere or is installed (real.get_include()).

CMAKE  ?= cmake
CTEST  ?= ctest
BUILD  := build
PYTHON ?= python3
PYRUN  := PYTHONPATH=$(CURDIR)/python $(PYTHON)
# Parallelism: detected core count (override with JOBS=N).
JOBS   ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

REAL_INCLUDE ?= $(CURDIR)/../real-v1/include

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
FORMAT_FILES := $(shell find include tests -name '*.hpp' -o -name '*.cpp')

.PHONY: all build test sanitize coverage coverage-build coverage-html \
        lint misra doc doc-no-coverage format format-check full-local-gate \
        python python-test bench install uninstall release clean help

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
	@echo "  make bench      Wall-time micro-benchmarks vs Python re (informational)"
	@echo "  make full-local-gate  Every gate in one command (the macOS gate of record)"
	@echo "  make install    Install the Python package (pip)"
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
	@ls tests/*.cpp | xargs -P $(JOBS) -I{} clang-tidy {} -- $(CXXSTD) $(INCLUDES)

misra:
	mkdir -p $(BUILD)
	printf '#include <scilex/scilex.hpp>\nint main(){ const scilex::lexer l({{0, real::regex("a")}}); return l.tokenize("a").size() == 1 ? 0 : 1; }\n' > $(BUILD)/misra_tu.cpp
	clang-tidy --config-file=.clang-tidy-misra \
	    --line-filter='[{"name":"misra_tu.cpp","lines":[[1,1]]}]' \
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
	uncrustify -c uncrustify.cfg --replace --no-backup $(FORMAT_FILES)

# Python binding: an abi3 CPython extension (Limited API) over the C++ lexer.
# REAL's headers are located via the installed real package, or the sibling
# checkout (see setup.py); the C++ library itself stays header-only.
python:
	$(PYTHON) setup.py -q build_ext --inplace

python-test: python
	$(PYRUN) -m unittest discover -s python/tests

# Wall-time micro-benchmarks vs Python's re (informational; never gated). See BENCHMARKS.md.
bench: python
	$(PYRUN) benchmarks/bench.py

# The complete local quality gate in one command — the canonical pre-push check
# and, since CI no longer runs macOS (see .github/workflows/ci.yml), the macOS gate
# of record. Runs every check this machine owns and fails on any issue, including a
# lint warning or any coverage dimension below 100%. If an incremental coverage build
# is ever suspect, `rm -rf $(COV_DIR)` first to force a clean re-measure.
full-local-gate:
	@$(MAKE) format-check
	@$(MAKE) test
	@$(MAKE) test CXX=g++-14 BUILD=$(BUILD)/gcc
	@$(MAKE) sanitize
	@$(MAKE) misra
	@$(MAKE) doc-no-coverage
	@$(MAKE) python-test
	@$(MAKE) lint | tee $(BUILD)/lint.log; ! grep -qE 'warning:|error:' $(BUILD)/lint.log
	@$(MAKE) coverage | tee $(BUILD)/coverage.log
	# Gate on a flag, not a bare exit 1 in the rule: that exit is overridden by END's exit,
	# so the gate silently accepted coverage below 100% 4D before this fix.
	@awk '/^TOTAL/{seen=1; if (gsub(/100\.00%/, "&") != 4) bad=1} END{exit (seen && !bad) ? 0 : 1}' $(BUILD)/coverage.log \
	  || { echo "full-local-gate: coverage is below 100% on some dimension — see above"; exit 1; }
	@echo "full-local-gate: ALL gates green (clang + g++-14, sanitize, MISRA, lint, doc, python, 100% coverage)"

format-check:
	uncrustify -c uncrustify.cfg --check $(FORMAT_FILES)

install:
	$(PYTHON) -m pip install .

uninstall:
	$(PYTHON) -m pip uninstall -y scilex

# Cuts a calendar-versioned release: computes YYYY.M.PATCH with the patch reset
# each month (first release of a month is .0; PEP 440 drops leading zeros, so
# 2026.6.1, never 2026.06.001), bumps both version files, commits, tags and
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
