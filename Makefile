# ==============================================================================
# Haze — standalone build entry
# ==============================================================================
# Convention: dbuild/ for Debug, build/ for Release
#
# RYANPR: Instead of this, make a help phony target to describe that is happening. Include example of running the integration test when the nbcc_fhetch_replay is being run from the niobium-client repository (if that makes sense)
# Standalone build (haze owns its own niobium-fhetch + openfhe checkout):
#   make sync                      # one-time: init vendor/niobium-fhetch + nested submodules
#   make build-release             # configure + build everything (Release)
#   make test-unit-release         # run the unit suite (no compiler dep)
#   make test-integration-release NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler
#                                  # run integration suite (requires nbcc_fhetch_replay binary)
#
# Parent-driven build (e.g. niobium-client adds_subdirectory(vendor/niobium-haze)):
#   The parent's CMake build system already handles configuring and building
#   haze. This Makefile is only used for standalone work.
#
# RYANPR: Make sure this is up to date
# Override knobs (parent or user can supply):
#   NIOBIUM_HAZE_FHETCH_DIR  External niobium-fhetch source tree to use instead
#                            of vendor/niobium-fhetch. When set, vendor/niobium-fhetch
#                            does not need to be initialized.
#   OPENFHE_INSTALL_DIR      Where OpenFHE is installed (libs + headers).
#                            Defaults to <fhetch>/vendor/lib/openfhe.
#   JSON_INCLUDE_DIR         nlohmann/json single_include directory.
#                            Empty -> use niobium-fhetch's vendored copy.
#   EXTERNAL_OPENFHE         1 if a parent built OpenFHE and pointed
#                            OPENFHE_INSTALL_DIR at it; we skip the openfhe
#                            build target chain.
#   NIOBIUM_COMPILER_ROOT    Path to a niobium-compiler checkout containing
#                            build/nbcc_fhetch_replay. Required for integration
#                            tests; the compiler binary is invoked directly
#                            (no HTTP transport from haze's standalone path).
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
  OPENFHE_BUILD_DEP_DEBUG   :=
  OPENFHE_BUILD_DEP_RELEASE :=
else
  OPENFHE_BUILD_DEP_DEBUG   := build-openfhe
  OPENFHE_BUILD_DEP_RELEASE := build-openfhe-release
endif

# CMake -D flags emitted only when the corresponding override is set.
CMAKE_FHETCH_DIR_FLAG       := $(if $(NIOBIUM_HAZE_FHETCH_DIR),-DNIOBIUM_HAZE_FHETCH_DIR="$(NIOBIUM_HAZE_FHETCH_DIR)")
CMAKE_JSON_INCLUDE_DIR_FLAG := $(if $(JSON_INCLUDE_DIR),-DJSON_INCLUDE_DIR="$(JSON_INCLUDE_DIR)")

# Path to a built niobium-compiler checkout (must contain
# build/nbcc_fhetch_replay). Haze does NOT vendor the compiler — set this
# explicitly when running integration tests:
#   make test-integration-release NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler
# The fallback `vendor/niobium-compiler` lets developers symlink an
# external checkout in if they prefer that pattern; otherwise the
# test-integration-release prerequisite check will print a clear error.
# RYANPR: Only have the variable based root, not some magic symlink path.
NIOBIUM_COMPILER_ROOT ?= $(CURDIR)/vendor/niobium-compiler

# ==============================================================================
# Build config helper (mirrors niobium-client/Makefile)
# ==============================================================================

BUILD_CONFIG = Debug
BUILD_DIR = dbuild

# RYANPR: What is this for?
define set-build-config
$(eval BUILD_CONFIG = $(1))
$(eval BUILD_DIR = $(2))
endef

# Per-test artifact runs dir (tests cd here so libnbfhetch's program_dir
# resolves under build/ and not into the source tree).
HAZE_RUNS_DIR = $(CURDIR)/$(BUILD_DIR)/runs

# ==============================================================================
# Phony targets
# ==============================================================================

.PHONY: help sync \
        config config-release build build-release release \
        config-openfhe config-openfhe-release build-openfhe build-openfhe-release \
        test-unit-release test-integration-release test-release \
        clean clean-runs

##@ Primary Targets

# RYANPR: What the hell is this. Ideally we just include the help here.
help: ## Display this help message
	@awk 'BEGIN {FS = ":.*##"; printf "\nUsage:\n  make \033[36m<target>\033[0m\n"} /^[a-zA-Z_0-9-]+:.*?##/ { printf "  \033[36m%-28s\033[0m %s\n", $$1, $$2 } /^##@/ { printf "\n\033[1m%s\033[0m\n", substr($$0, 5) } ' $(MAKEFILE_LIST)

##@ Submodule sync (standalone use only)

sync: ## Init vendor/niobium-fhetch + its nested openfhe / json submodules (recursive)
	git submodule update --init --recursive vendor/niobium-fhetch

##@ OpenFHE Build (skipped when EXTERNAL_OPENFHE=1)

OPENFHE_CMAKE_FLAGS = \
	-DBUILD_SHARED=ON \
	-DBUILD_EXAMPLES=OFF \
	-DBUILD_UNITTESTS=OFF \
	-DBUILD_BENCHMARKS=OFF \
	-DBUILD_EXTRAS=OFF \
	-DWITH_CPROBES=ON \
	-DWITH_OPENMP=OFF

# RYANPR: Think about a way to make all these `X` and `X-release` targets modular, since otherwise we are repeating code all the time. Essentially there should be a build directory, a build symbol (Debug/Release), and you can pipe those through the appropriate places. `set-build-config` kinda does parts of this but doesn't seem to use them. Apply to all targets that this works agains (config, config-openfhe, build, build-openfhe, etc) Use a variable that defaults to one mode and override it on the command line. For example:
#
#   MODE ?= debug
#   CFLAGS_debug   := -g -O0
#   CFLAGS_release := -O2 -DNDEBUG
#
#   build:
#         $(CC) $(CFLAGS_$(MODE)) -o app main.c
config-openfhe: ## Configure OpenFHE (Debug)
	@if [ ! -d "$(OPENFHE_DIR)" ]; then \
		echo "ERROR: $(OPENFHE_DIR) is empty. Run 'make sync' first."; exit 2; \
	fi
	cd "$(OPENFHE_DIR)" && cmake -S . -B dbuild \
		-DCMAKE_BUILD_TYPE=Debug \
		$(OPENFHE_CMAKE_FLAGS) \
		-DCMAKE_INSTALL_PREFIX="$(OPENFHE_INSTALL_DIR)"

config-openfhe-release: ## Configure OpenFHE (Release)
	@if [ ! -d "$(OPENFHE_DIR)" ]; then \
		echo "ERROR: $(OPENFHE_DIR) is empty. Run 'make sync' first."; exit 2; \
	fi
	cd "$(OPENFHE_DIR)" && cmake -S . -B build \
		-DCMAKE_BUILD_TYPE=Release \
		$(OPENFHE_CMAKE_FLAGS) \
		-DCMAKE_INSTALL_PREFIX="$(OPENFHE_INSTALL_DIR)"

build-openfhe: config-openfhe ## Build and install OpenFHE locally (Debug)
	cd "$(OPENFHE_DIR)" && cmake --build dbuild -j $(NUM_CPUS) --target install --config Debug

build-openfhe-release: config-openfhe-release ## Build and install OpenFHE locally (Release)
	cd "$(OPENFHE_DIR)" && cmake --build build -j $(NUM_CPUS) --target install --config Release

##@ Haze Build

config: $(OPENFHE_BUILD_DEP_DEBUG) ## Configure haze (Debug)
	$(call set-build-config,Debug,dbuild)
	cmake -S "$(CURDIR)" -B "$(CURDIR)/dbuild" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DOPENFHE_INSTALL_DIR="$(OPENFHE_INSTALL_DIR)" \
		$(CMAKE_FHETCH_DIR_FLAG) \
		$(CMAKE_JSON_INCLUDE_DIR_FLAG)

config-release: $(OPENFHE_BUILD_DEP_RELEASE) ## Configure haze (Release)
	$(call set-build-config,Release,build)
	cmake -S "$(CURDIR)" -B "$(CURDIR)/build" \
		-DCMAKE_BUILD_TYPE=Release \
		-DOPENFHE_INSTALL_DIR="$(OPENFHE_INSTALL_DIR)" \
		$(CMAKE_FHETCH_DIR_FLAG) \
		$(CMAKE_JSON_INCLUDE_DIR_FLAG)

build: config ## Build haze (Debug)
	$(call set-build-config,Debug,dbuild)
	cmake --build dbuild -j $(NUM_CPUS) --config Debug

build-release: config-release ## Build haze (Release)
	$(call set-build-config,Release,build)
	cmake --build build -j $(NUM_CPUS) --config Release

##@ Haze Tests

# RYANPR: Why do the tests not run against the debug copy?
test-unit-release: build-release ## Run the haze unit suite (HAZE_TARGET=local; no compiler dep)
	$(call set-build-config,Release,build)
	@rm -rf "$(HAZE_RUNS_DIR)/haze"
	@mkdir -p "$(HAZE_RUNS_DIR)"
	@cd "$(HAZE_RUNS_DIR)" && \
	  HAZE_TARGET=local "$(CURDIR)/build/haze_tests" "~[integration]"

test-integration-release: build-release ## End-to-end haze integration tests (direct nbcc_fhetch_replay invocation; no HTTP transport)
	$(call set-build-config,Release,build)
	@if [ ! -x "$(NIOBIUM_COMPILER_ROOT)/build/nbcc_fhetch_replay" ]; then \
		echo "ERROR: nbcc_fhetch_replay not found at $(NIOBIUM_COMPILER_ROOT)/build/nbcc_fhetch_replay"; \
		echo "Build it with: (cd $(NIOBIUM_COMPILER_ROOT) && make release)"; \
		echo "Or override:   make test-integration-release NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler"; \
		exit 2; \
	fi
	@HAZE_TEST_BIN="$(CURDIR)/build/haze_tests" \
	 NIOBIUM_COMPILER_ROOT="$(NIOBIUM_COMPILER_ROOT)" \
	 OPENFHE_LIB="$(OPENFHE_INSTALL_DIR)/lib" \
	 HAZE_RUNS_DIR="$(HAZE_RUNS_DIR)" \
	 scripts/test_haze_integration_standalone.sh

test-release: test-unit-release test-integration-release ## Run all haze tests (unit + standalone integration)

##@ Cleanup

clean-runs: ## Remove haze test runs/ artifacts (keeps build outputs intact)
	-rm -rf "$(CURDIR)/build/runs" "$(CURDIR)/dbuild/runs"

# RYANPR: Should this not use clean-runs as a dependency?
clean: ## Remove all build artifacts (keeps vendor/ checkouts; refuses to touch external trees pointed at via NIOBIUM_HAZE_FHETCH_DIR or EXTERNAL_OPENFHE)
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
