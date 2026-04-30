# ==============================================================================
# Haze — standalone build entry
# ==============================================================================
# Build directory convention: dbuild/ for MODE=debug, build/ for MODE=release.
# All targets honour MODE; defaults to release. See `make help`.
#
# Override knobs (parent or user can supply):
#   MODE                     debug | release. Selects build dir (dbuild|build)
#                            and CMake config (Debug|Release). Default: release.
#   NUM_CPUS                 Build parallelism. Auto-detected from sysctl/nproc;
#                            override to throttle.
#   NIOBIUM_HAZE_FHETCH_DIR  External niobium-fhetch source tree to use instead
#                            of vendor/niobium-fhetch. When set, vendor/niobium-fhetch
#                            does not need to be initialised.
#   OPENFHE_INSTALL_DIR      Where OpenFHE is installed (libs + headers).
#                            Defaults to <fhetch>/vendor/lib/openfhe.
#   JSON_INCLUDE_DIR         nlohmann/json single_include directory.
#                            Empty -> use niobium-fhetch's vendored copy.
#   EXTERNAL_OPENFHE         1 if a parent built OpenFHE and pointed
#                            OPENFHE_INSTALL_DIR at it; we skip the openfhe
#                            build target chain.
#   NIOBIUM_COMPILER_ROOT    Path to a niobium-compiler checkout containing
#                            build/nbcc_fhetch_replay. Required for
#                            test-transport; the compiler binary is invoked
#                            directly (no HTTP transport from haze's
#                            standalone path). No default.
# ==============================================================================

SHELL := /bin/bash
.SHELLFLAGS := -o pipefail -c

# ==============================================================================
# Platform / CPU detection
# ==============================================================================

UNAME_S := $(shell uname -s)

ifndef NUM_CPUS
  ifeq ($(UNAME_S), Darwin)
    NUM_CPUS := $(shell sysctl -n hw.ncpu)
  else
    NUM_CPUS := $(shell nproc)
  endif
endif

# ==============================================================================
# Mode selection (debug vs release)
# ==============================================================================

MODE ?= release

BUILD_DIR_debug   := dbuild
BUILD_DIR_release := build
BUILD_DIR := $(BUILD_DIR_$(MODE))

CMAKE_CONFIG_debug   := Debug
CMAKE_CONFIG_release := Release
CMAKE_CONFIG := $(CMAKE_CONFIG_$(MODE))

ifeq ($(BUILD_DIR),)
  $(error invalid MODE='$(MODE)' (expected 'debug' or 'release'))
endif

# ==============================================================================
# Paths
# ==============================================================================

# niobium-fhetch resolution: same precedence as haze's CMakeLists.txt.
# 1. Caller-provided override
# 2. haze's own vendor submodule
NIOBIUM_HAZE_FHETCH_DIR ?=
ifeq ($(NIOBIUM_HAZE_FHETCH_DIR),)
  FHETCH_DIR := $(CURDIR)/vendor/niobium-fhetch
else
  FHETCH_DIR := $(NIOBIUM_HAZE_FHETCH_DIR)
endif

# OpenFHE: defaults to niobium-fhetch's vendored install, but a parent
# (or user) can point us at a pre-built install with EXTERNAL_OPENFHE=1
# + OPENFHE_INSTALL_DIR=<path>.
OPENFHE_DIR         ?= $(FHETCH_DIR)/vendor/openfhe
OPENFHE_INSTALL_DIR ?= $(FHETCH_DIR)/vendor/lib/openfhe
JSON_INCLUDE_DIR    ?=

EXTERNAL_OPENFHE ?= 0
ifeq ($(EXTERNAL_OPENFHE),1)
  OPENFHE_BUILD_DEP :=
else
  OPENFHE_BUILD_DEP := build-openfhe
endif

# CMake -D flags emitted only when the corresponding override is set.
CMAKE_FHETCH_DIR_FLAG       := $(if $(NIOBIUM_HAZE_FHETCH_DIR),-DNIOBIUM_HAZE_FHETCH_DIR="$(NIOBIUM_HAZE_FHETCH_DIR)")
CMAKE_JSON_INCLUDE_DIR_FLAG := $(if $(JSON_INCLUDE_DIR),-DJSON_INCLUDE_DIR="$(JSON_INCLUDE_DIR)")

# Path to a built niobium-compiler checkout (must contain
# build/nbcc_fhetch_replay). Haze does NOT vendor the compiler — set this
# explicitly when running transport-path tests:
#   make test-transport NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler
NIOBIUM_COMPILER_ROOT ?=

# Per-test artifact runs dir (tests cd here so libnbfhetch's program_dir
# resolves under build/ and not into the source tree).
HAZE_RUNS_DIR = $(CURDIR)/$(BUILD_DIR)/runs

# ==============================================================================
# Phony targets
# ==============================================================================

.PHONY: help sync \
        config build \
        config-openfhe build-openfhe \
        test-unit test-sim test-transport test test-all \
        clean clean-runs

# ==============================================================================
# help
# ==============================================================================

define HAZE_HELP_TEXT

Usage: make <target> [MODE=debug|release]

  Build:
    config              Configure haze (uses MODE; default: release)
    build               Build haze
    config-openfhe      Configure OpenFHE
    build-openfhe       Build and install OpenFHE locally
    sync                Init vendor/niobium-fhetch submodule

  Test:
    test-unit           Run unit suite (HAZE_TARGET=local; no FHE math)
    test-sim            Run sim suite (in-process fhetch_sim)
    test-transport      Run integration suite via nbcc_fhetch_replay
                        (requires NIOBIUM_COMPILER_ROOT)
    test                Default: test-unit + test-sim
    test-all            test-unit + test-sim + test-transport

  Cleanup:
    clean-runs          Remove test runs/ artifacts
    clean               Remove all build artifacts

  Examples:
    make test                                        # default tests
    make test MODE=debug                             # debug build
    make test-transport NIOBIUM_COMPILER_ROOT=/path  # transport path
    cd niobium-client && make test-haze              # parent-driven

endef
export HAZE_HELP_TEXT

help: ## Display this help message
	@echo "$$HAZE_HELP_TEXT"

# ==============================================================================
# Submodule sync (standalone use only)
# ==============================================================================

sync: ## Init vendor/niobium-fhetch + its nested openfhe / json submodules (recursive)
	git submodule update --init --recursive vendor/niobium-fhetch

# ==============================================================================
# OpenFHE Build (skipped when EXTERNAL_OPENFHE=1)
# ==============================================================================

OPENFHE_CMAKE_FLAGS = \
	-DBUILD_SHARED=ON \
	-DBUILD_EXAMPLES=OFF \
	-DBUILD_UNITTESTS=OFF \
	-DBUILD_BENCHMARKS=OFF \
	-DBUILD_EXTRAS=OFF \
	-DWITH_CPROBES=ON \
	-DWITH_OPENMP=OFF

config-openfhe: ## Configure OpenFHE
	@if [ ! -d "$(OPENFHE_DIR)" ]; then \
		echo "ERROR: $(OPENFHE_DIR) is empty. Run 'make sync' first."; exit 2; \
	fi
	cd "$(OPENFHE_DIR)" && cmake -S . -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=$(CMAKE_CONFIG) \
		$(OPENFHE_CMAKE_FLAGS) \
		-DCMAKE_INSTALL_PREFIX="$(OPENFHE_INSTALL_DIR)"

build-openfhe: config-openfhe ## Build and install OpenFHE locally
	cd "$(OPENFHE_DIR)" && cmake --build "$(BUILD_DIR)" -j $(NUM_CPUS) --target install --config $(CMAKE_CONFIG)

# ==============================================================================
# Haze Build
# ==============================================================================

config: $(OPENFHE_BUILD_DEP) ## Configure haze (uses MODE)
	cmake -S "$(CURDIR)" -B "$(CURDIR)/$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=$(CMAKE_CONFIG) \
		-DOPENFHE_INSTALL_DIR="$(OPENFHE_INSTALL_DIR)" \
		$(CMAKE_FHETCH_DIR_FLAG) \
		$(CMAKE_JSON_INCLUDE_DIR_FLAG)

build: config ## Build haze (uses MODE)
	cmake --build "$(BUILD_DIR)" -j $(NUM_CPUS) --config $(CMAKE_CONFIG)

# ==============================================================================
# Haze Tests
# ==============================================================================

test-unit: build ## Run unit suite (HAZE_TARGET=local; no FHE math)
	@rm -rf "$(HAZE_RUNS_DIR)/haze"
	@mkdir -p "$(HAZE_RUNS_DIR)"
	@cd "$(HAZE_RUNS_DIR)" && \
	  HAZE_TARGET=local "$(CURDIR)/$(BUILD_DIR)/haze_tests" "~[integration]"

test-sim: build ## Run sim suite (in-process fhetch_sim; validates FHE math)
	@rm -rf "$(HAZE_RUNS_DIR)/haze"
	@mkdir -p "$(HAZE_RUNS_DIR)"
	@# Target literal must match haze::kFhetchSimTarget in src/core/config.hpp
	@# AND the haze_sim_tests ENVIRONMENT entry in CMakeLists.txt.
	@cd "$(HAZE_RUNS_DIR)" && \
	  HAZE_TARGET=fhetch_sim "$(CURDIR)/$(BUILD_DIR)/haze_tests" "[integration]"

test-transport: build ## Run integration suite via nbcc_fhetch_replay (opt-in)
	@if [ -z "$(NIOBIUM_COMPILER_ROOT)" ]; then \
		echo "ERROR: NIOBIUM_COMPILER_ROOT is not set."; \
		echo "Run: make test-transport NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler"; \
		exit 2; \
	fi
	@if [ ! -x "$(NIOBIUM_COMPILER_ROOT)/build/nbcc_fhetch_replay" ]; then \
		echo "ERROR: nbcc_fhetch_replay not found at $(NIOBIUM_COMPILER_ROOT)/build/nbcc_fhetch_replay"; \
		echo "Build it with: (cd $(NIOBIUM_COMPILER_ROOT) && make release)"; \
		exit 2; \
	fi
	@HAZE_TEST_BIN="$(CURDIR)/$(BUILD_DIR)/haze_tests" \
	 NIOBIUM_COMPILER_ROOT="$(NIOBIUM_COMPILER_ROOT)" \
	 OPENFHE_LIB="$(OPENFHE_INSTALL_DIR)/lib" \
	 HAZE_RUNS_DIR="$(HAZE_RUNS_DIR)" \
	 HAZE_TRANSPORT_TARGET=FUNC_SIM \
	 scripts/test_haze_integration_standalone.sh

test: test-unit test-sim ## Run default test suites (no transport dependency)

test-all: test-unit test-sim test-transport ## Run everything (incl. transport path)

# ==============================================================================
# Cleanup
# ==============================================================================

clean-runs: ## Remove haze test runs/ artifacts (keeps build outputs intact)
	-rm -rf "$(CURDIR)/build/runs" "$(CURDIR)/dbuild/runs"

clean: clean-runs ## Remove all build artifacts (keeps vendor/ checkouts; refuses to touch external trees pointed at via NIOBIUM_HAZE_FHETCH_DIR or EXTERNAL_OPENFHE)
	-rm -rf "$(CURDIR)/build" "$(CURDIR)/dbuild"
	@if [ "$(EXTERNAL_OPENFHE)" != "1" ]; then \
	  case "$(OPENFHE_DIR)" in \
	    "$(CURDIR)"/*) rm -rf "$(OPENFHE_DIR)/build" "$(OPENFHE_DIR)/dbuild" ;; \
	    *) echo "skipping clean of external OPENFHE_DIR=$(OPENFHE_DIR)" ;; \
	  esac; \
	  case "$(OPENFHE_INSTALL_DIR)" in \
	    "$(CURDIR)"/*) rm -rf "$(OPENFHE_INSTALL_DIR)" ;; \
	    *) echo "skipping clean of external OPENFHE_INSTALL_DIR=$(OPENFHE_INSTALL_DIR)" ;; \
	  esac; \
	else \
	  echo "skipping clean of OpenFHE build/install dirs (EXTERNAL_OPENFHE=1)"; \
	fi
