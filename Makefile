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

REAL_INCLUDE ?= $(CURDIR)/../real-v1/include

ifeq ($(origin CXX),command line)
CMAKE_CXX := -DCMAKE_CXX_COMPILER=$(CXX)
endif

CMAKE_REAL   := -DSCILEX_REAL_INCLUDE=$(REAL_INCLUDE)
CXXSTD       := -std=c++20
# REAL is a dependency: include it as a system header so the linters analyze
# SciLex's own code only (REAL passes its own gates).
INCLUDES     := -Iinclude -isystem $(REAL_INCLUDE)
FORMAT_FILES := $(shell find include tests -name '*.hpp' -o -name '*.cpp')

.PHONY: all build test sanitize coverage coverage-build coverage-html \
        lint misra doc doc-no-coverage format format-check clean help

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
	@echo "  make clean      Remove build artifacts"
	@echo ""
	@echo "  Override the compiler:   make test CXX=g++-14"
	@echo "  Override REAL's headers: make test REAL_INCLUDE=/path/to/real/include"

all: build

# --- build / test (delegated to CMake) ------------------------------------

build:
	$(CMAKE) -S . -B $(BUILD) $(CMAKE_CXX) $(CMAKE_REAL) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD) -j

test: build
	$(CTEST) --test-dir $(BUILD) --output-on-failure

sanitize:
	$(CMAKE) -S . -B $(BUILD)/sanitize $(CMAKE_CXX) $(CMAKE_REAL) -DSCILEX_SANITIZE=ON
	$(CMAKE) --build $(BUILD)/sanitize -j
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
	$(CMAKE) --build $(COV_DIR) -j
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
	clang-tidy $(wildcard tests/*.cpp) -- $(CXXSTD) $(INCLUDES)

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

format-check:
	uncrustify -c uncrustify.cfg --check $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)
