# Haze — CUDA-like API for Niobium FHE

A C library that exposes a **CUDA-like** API for the **Niobium Mistic** FHE
accelerator. Library authors write `hazeMalloc` / `hazeMemcpy` / `hazeAdd` /
`hazeNTT` calls the same way they would write `cudaMalloc` / `cudaMemcpy` /
custom CUDA kernels, and haze records every operation as an unoptimized FHETCH
Polynomial IR trace that is then executed locally (for validation) or shipped to
the Niobium compilation service for optimization and deployment to hardware.

Where [`niobium-client`](https://github.com/NiobiumInc/niobium-client) integrates
at the OpenFHE level (probes intercept `EvalMult`, `EvalAdd`, ...) haze
integrates one layer below: each public entry point is a single polynomial-level
op (`NTT`, `MUL`, `ADDP`, basis convert, ...). The shape is deliberately CUDA's
so that GPU FHE libraries written against CUDA — for example
[FIDESlib](https://github.com/CKKS-Community/FIDESlib) — can be retargeted to
Niobium hardware with minimal porting effort.

For the FHETCH Polynomial IR instruction set, session API, trace format, and
simulator internals, see the companion repository:
[`niobium-fhetch`](https://github.com/NiobiumInc/niobium-fhetch).

## How it fits together

```
 Customer C / C++ Application
 (uses CUDA-shaped haze API: hazeMalloc, hazeAdd, hazeNTT, ...)
         |
         | #include <haze/haze.h>; links against libhaze
         v
 +--------------------------------------------------+
 |  libhaze  (this repo)                            |
 |  haze.h   — CUDA-shaped C entry points           |
 |  every call becomes one FHETCH Polynomial IR op  |
 +--------------------------------------------------+
         |
         | delegates recording to
         v
 +--------------------------------------------------+
 |  niobium-fhetch (libnbfhetch)                    |
 |   compiler() singleton + fhetch::* recording API |
 +--------------------------------------------------+
         |
         | hazeMemcpy(D2H) (or explicit hazeReplay())
         | finalises the .fhetch trace + manifest
         v
         +-------------------+--------------------+
         |                                        |
         v                                        v
 +-----------------------+            +----------------------+
 | local (default)       |            | Niobium Server       |
 | — in-process FHETCH   |            | (niobium-compiler)   |
 |   simulator inside    |            | — proprietary        |
 |   libnbfhetch         |            | — optimizes and      |
 | — populates D2H       |            |   deploys to Mistic, |
 |   reads with sim'd    |            |   FUNC_SIM, FHE_SIM, |
 |   ciphertext values   |            |   FPGA_TRI, ...      |
 +-----------------------+            +----------------------+
```

### Step by step

1. **Compile & Link** — The application includes `<haze/haze.h>` and links
   against `libhaze`. No probes, no instrumented OpenFHE. Haze pulls
   `libnbfhetch` in transitively for the recording back-end.

2. **Configure** — `hazeSetRingDimension(N)`, `hazeSetCiphertextModulus(idx, q)`
   for each residue, and `hazeConfigureDevice()` lock in the FHE parameter set.
   Optional: `hazeSetTarget("FUNC_SIM")` (or set `HAZE_TARGET` in the
   environment) to pick a non-default replay target.

3. **Allocate & Record** — `hazeMalloc` returns one FHETCH-addressable
   polynomial. Compute calls (`hazeAdd`, `hazeMul`, `hazeNTT`, `hazeAutomorph`,
   `hazeBasisConvert`, ...) record one or more FHETCH instructions per call;
   nothing executes yet. Stream and event handles are accepted for CUDA-shape
   parity but are intentionally no-ops — recording has no notion of
   stream-relative ordering until the trace is flushed.

4. **Replay & Read** — `hazeMemcpy(host_buf, dev_ptr, n, HAZE_MEMCPY_DEVICE_TO_HOST)`
   finalises the current epoch's `.fhetch` trace, dispatches it to the
   configured target, and copies the simulator-computed values into the host
   buffer. `hazeReplay()` is also exposed for callers that want to flush the
   recording without an immediate readback.

5. **Submit (production)** — Choose a compiler-side target such as `FPGA_TRI`
   (or `FUNC_SIM` / `FHE_SIM` / `fhetch_sim`) before replay. Haze's transport
   layer ships the recorded trace plus serialized inputs to a running
   `nbcc_fhetch_replay` instance, which exercises the Niobium compilation
   pipeline and either simulates or executes on real hardware.

## Recording an FHE op

The example below configures the device, records a pointwise add of two
polynomials, replays through the in-process simulator, and reads the result:

```c
#include <haze/haze.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    const uint64_t ring_dim = 4096;
    const size_t   bytes    = ring_dim * sizeof(uint64_t);
    const uint64_t modulus  = 576460752303415297ULL;
    const int      mod_idx  = 0;

    /* ---- Configure the FHE parameter set. ---- */
    hazeSetRingDimension(ring_dim);
    hazeSetCiphertextModulus(mod_idx, modulus);
    hazeConfigureDevice();

    /* ---- Allocate three device polynomials. ---- */
    void *d_a = NULL, *d_b = NULL, *d_dst = NULL;
    hazeMalloc(&d_a,   bytes);
    hazeMalloc(&d_b,   bytes);
    hazeMalloc(&d_dst, bytes);

    /* ---- Stage host inputs and record the add. ---- */
    uint64_t a[ring_dim], b[ring_dim], result[ring_dim];
    for (uint64_t i = 0; i < ring_dim; ++i) { a[i] = 1; b[i] = 2; }
    hazeMemcpy(d_a, a, bytes, HAZE_MEMCPY_HOST_TO_DEVICE);
    hazeMemcpy(d_b, b, bytes, HAZE_MEMCPY_HOST_TO_DEVICE);

    hazeAdd(d_dst, d_a, d_b, mod_idx, /*stream=*/NULL);

    /* ---- D2H flushes the recording and reads the result back. ---- */
    hazeMemcpy(result, d_dst, bytes, HAZE_MEMCPY_DEVICE_TO_HOST);

    printf("result[0] = %llu\n", (unsigned long long)result[0]);  /* 3 */

    hazeFree(d_a);
    hazeFree(d_b);
    hazeFree(d_dst);
    return 0;
}
```

Replace `hazeAdd` with any sequence of `hazeMul`, `hazeNTT`, `hazeAutomorph`,
`hazeBasisConvert`, ... and the recording layer captures every op in execution
order. `test/test_compute.cpp` and `test/test_basis_convert.cpp` exercise the
full surface; they are useful as worked examples.

## Building

### Standalone (non-nix)

```sh
git submodule update --init --recursive   # or: make sync
make build MODE=release                   # Release into build/ (the default)
make build MODE=debug                     # Debug   into dbuild/
```

`MODE` defaults to `release`, so a bare `make build` is equivalent to
`make build MODE=release`. The same `MODE=` selector applies to every target
that produces or consumes build artefacts (`config`, `build`, `test`,
`test-unit`, `test-sim`, `test-transport`, `test-all`, `clean`).

The top-level `Makefile` builds OpenFHE (vendored at
`vendor/niobium-fhetch/vendor/openfhe`), installs it under
`vendor/niobium-fhetch/vendor/lib/openfhe`, then builds `libhaze` and
`haze_tests` against `Niobium::fhetch`. The first build is slow because OpenFHE
is compiled from source; subsequent invocations skip it.

To skip the OpenFHE build entirely (e.g. when a parent project already installed
it):

```sh
EXTERNAL_OPENFHE=1 OPENFHE_INSTALL_DIR=/path/to/openfhe make build MODE=release
```

#### Prerequisites

- clang >= 19 (C++23 required by `CMakeLists.txt`).
- cmake >= 3.22.
- Catch2 3.x.
- git (submodule init).

```sh
# macOS (Homebrew)
brew install cmake catch2 llvm@19

# Debian / Ubuntu (Catch2 3.x may need a backport or source build)
sudo apt install cmake catch2 clang-19 llvm-19-dev
```

#### Make targets

```
Build:
  sync              Init vendor/niobium-fhetch (recursive).
  config            Configure haze (uses MODE).
  build             Build haze (uses MODE).
  config-openfhe    Configure OpenFHE.
  build-openfhe     Build and install OpenFHE locally.

Test:
  test-unit         Unit suite (HAZE_TARGET=local; no FHE math).
  test-sim          [integration] suite via the in-process FHETCH
                    simulator (HAZE_TARGET=local). Validates FHE math
                    without external binaries.
  test-transport    [integration] suite via nbcc_fhetch_replay
                    (opt-in; requires NIOBIUM_COMPILER_ROOT).
  test              Default: test-unit + test-sim.
  test-all          test-unit + test-sim + test-transport.

Cleanup:
  clean-runs        Remove test runs/ artifacts.
  clean             Remove all build artifacts (refuses to touch
                    external trees pointed at via
                    NIOBIUM_HAZE_FHETCH_DIR or EXTERNAL_OPENFHE).
```

`make help` prints the same list at runtime.

#### Override knobs

Make variables and / or environment:

| Variable                  | Purpose                                                                                                     | Default                                   |
| ------------------------- | ----------------------------------------------------------------------------------------------------------- | ----------------------------------------- |
| `MODE`                    | `debug` or `release`. Selects `dbuild`/`build` and CMake `Debug`/`Release`.                                 | `release`                                 |
| `NUM_CPUS`                | Build parallelism.                                                                                          | Auto (`sysctl -n hw.ncpu` / `nproc`).     |
| `NIOBIUM_HAZE_FHETCH_DIR` | External `niobium-fhetch` source tree to use instead of `vendor/niobium-fhetch`.                            | unset (vendor submodule).                 |
| `OPENFHE_INSTALL_DIR`     | Where OpenFHE is installed (libs + headers).                                                                | `<fhetch>/vendor/lib/openfhe`.            |
| `EXTERNAL_OPENFHE`        | `1` skips the OpenFHE build chain (caller supplies it via `OPENFHE_INSTALL_DIR`).                           | `0`.                                      |
| `JSON_INCLUDE_DIR`        | `nlohmann/json` single-include directory.                                                                   | unset (use niobium-fhetch's vendor copy). |
| `NIOBIUM_COMPILER_ROOT`   | Path to a `niobium-compiler` checkout containing `build/nbcc_fhetch_replay`. Required for `test-transport`. | unset.                                    |

Runtime selector (consumed by `libhaze` itself, not the Makefile):

| Variable      | Purpose                                                                                                                                                                                                                                                     |
| ------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `HAZE_TARGET` | Replay target: `local` (default; in-process simulator) or one of `FHE_SIM`, `FUNC_SIM`, `FPGA_TRI`, `fhetch_sim` (HTTP transport to `nbcc_fhetch_replay`). Read on first `hazeReplay`. See [`include/haze/haze.h`](include/haze/haze.h) for the full table. |

CMake-level toggles (`-D...`):

| Option                                     | Default | Effect                                                               |
| ------------------------------------------ | ------- | -------------------------------------------------------------------- |
| `HAZE_SANITIZERS`                          | `OFF`   | ASAN + UBSAN.                                                        |
| `HAZE_TSAN`                                | `OFF`   | TSAN. Mutually exclusive with `HAZE_SANITIZERS`.                     |
| `HAZE_FBC_REDUCED_NOISE`                   | `ON`    | Test oracle uses OpenFHE's `ReducedNoise` FBC variant.               |
| `NIOBIUM_CLIENT_HAZE_WITH_TRANSPORT_TESTS` | `OFF`   | Register `haze_transport_tests` as a ctest entry (parent-build use). |

### As a `niobium-client` submodule

When haze is checked out under `niobium-client/vendor/niobium-haze`, the parent
owns the build graph and haze's own `Makefile` is not invoked. The parent
exposes wrapper targets in the `##@ Haze` section of its top-level `Makefile`.

Building (from the niobium-client root):

```sh
make build-release          # full client build; produces
                            # build/vendor/niobium-haze/{haze_tests,libhaze.*,
                            # libhaze_replay_bridge.*} via the parent CMake
                            # graph. NIOBIUM_CLIENT_WITH_HAZE=ON is the default.
make build-haze-release     # rebuild only the haze targets without touching
                            # the rest of the client (handy while iterating).
```

Testing (from the niobium-client root):

```sh
make test-haze-unit-release
# Same coverage as standalone `make test-unit`. Runs
# haze_tests "~[integration]" with HAZE_TARGET=local. No transport,
# no compiler binary required.

make test-haze-integration-release \
    NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler
# End-to-end transport round trip:
#   1. spawns build/src/fhetch_transport/nbcc_fhetch_replay_server,
#   2. puts the client-side forwarder first on PATH,
#   3. runs haze_tests "[integration]" with HAZE_TARGET=FUNC_SIM.
# Differs from standalone `make test-transport`, which dispatches to
# nbcc_fhetch_replay directly without the in-tree forwarder/server pair.

make test-haze-release      # both of the above.
make clean-haze             # drop build/vendor/niobium-haze/runs/ only.
```

Path differences vs the standalone build:

| Artifact      | Parent-driven                          | Standalone         |
| ------------- | -------------------------------------- | ------------------ |
| `haze_tests`  | `build/vendor/niobium-haze/haze_tests` | `build/haze_tests` |
| Test runs dir | `build/vendor/niobium-haze/runs/`      | `build/runs/`      |

### With the nix flake

`nix develop` enters `devShells.default` (`haze-dev`). The shell provides
clang 19, cmake, clang-tools (clang-format / clang-tidy / clangd), Catch2 3.x,
and jujutsu, with `MACOSX_DEPLOYMENT_TARGET=14.0` and a pinned `SDKROOT`. The
`shellHook` wires `CMAKE_PREFIX_PATH` and `LD_LIBRARY_PATH` for the
niobium-compiler / OpenFHE artefacts under `vendor/niobium-compiler` (when
present).

Inside the shell, the same Make targets work:

```sh
EXTERNAL_OPENFHE=1 make build MODE=release
```

To run a one-shot build without entering the shell interactively:

```sh
nix develop --command bash -c 'EXTERNAL_OPENFHE=1 make build MODE=release'
```

Heads-up on macOS: if OpenFHE was previously built outside `nix develop`
against the host SDK and haze is built inside the shell, the two libc++ ABIs
collide and tests segfault non-deterministically. See [`CLAUDE.md`](CLAUDE.md)
-> "SDK / ABI mismatch trap on macOS" for the rebuild recipe and verification
steps.

## Testing

Three suites, all built into a single `haze_tests` Catch2 binary and split by
tag plus environment:

- `test-unit` — state-machine and recording-only coverage. Runs every case not
  tagged `[integration]`. Does not call `hazeReplay` and does not validate FHE
  math results. Fast (~1 second).
- `test-sim` — `[integration]`-tagged cases run through libnbfhetch's
  in-process FHETCH simulator (`HAZE_TARGET=local`). Validates real FHE math
  without an external binary or HTTP transport.
- `test-transport` — same `[integration]` filter, but the recorded trace is
  shipped over HTTP to a `niobium-compiler`-built `nbcc_fhetch_replay`.
  Requires `NIOBIUM_COMPILER_ROOT` pointing at a checkout containing
  `build/nbcc_fhetch_replay`.

## Project structure

```
niobium-haze/
  include/haze/             # public C API (haze.h, haze_types.h)
  src/
    api/                    # extern-C boundary; one TU per public-header
                            # section (compute, memory, config, device,
                            # stream, lifecycle, error, basis_convert)
    core/                   # internal implementation: epoch, allocator,
                            # backend, config, basis convert, polynomial IO
    common/                 # shared leaf utilities (errors, log)
  replay_bridge/            # OpenFHE-using helper linked into libhaze;
                            # synthesises the cryptocontext + ciphertext
                            # templates the compiler-side replay needs
  test/                     # Catch2 3.x test suite
  scripts/                  # integration test harnesses
  vendor/
    niobium-fhetch/         # submodule: libnbfhetch + simulator + API
      vendor/openfhe/       # nested submodule
      vendor/json/          # nested submodule
  CMakeLists.txt            # libhaze + haze_tests + replay_bridge wiring
  Makefile                  # standalone driver (build, test-*, clean)
  flake.nix                 # nix dev shell + package
  CLAUDE.md                 # working notes (incl. macOS SDK trap recipe)
  README.md
```

## Architecture decisions

- **CUDA-like API by design** — Library authors targeting GPUs already think
  in `Malloc` / `Memcpy` / kernel calls. Haze preserves that shape so a CUDA
  FHE library can be retargeted to Niobium hardware by linking against `libhaze`
  instead of `libcudart`. Streams, events, and graph capture are stubbed to
  return success rather than removed, so porting code does not need to delete
  every reference to `cudaStream_t`.

- **Recording, not execution** — Every haze call appends to an in-memory
  FHETCH trace. Nothing executes until `hazeMemcpy(D2H)` (or an explicit
  `hazeReplay()`) flushes the recording. Stream-relative ordering is therefore
  not modelled; the recording is single-threaded by construction and the .fhetch
  trace itself defines the schedule the compiler optimises against.

- **Polynomial-level instead of OpenFHE-level** — `niobium-client` integrates
  at OpenFHE's `EvalAdd` / `EvalMult` boundary; haze sits one layer below at
  `NTT` / `INTT` / `Add` / `Mul` / `Automorph` / `BasisConvert`. Use haze when
  the caller wants explicit control over the polynomial schedule (e.g. porting
  a CUDA-resident FHE library); use `niobium-client` when the caller wants to
  run an unmodified OpenFHE app.

- **Replay bridge isolates OpenFHE** — `libhaze` itself is FHETCH-only and
  does not link OpenFHE into its public surface. `replay_bridge/` is a
  separately-built shared library that uses OpenFHE to synthesise the
  `cryptocontext.dat`, `ciphertext_templates/`, and per-input `.bin` / `.ids`
  artefacts the compiler-side `nbcc_fhetch_replay` needs to consume a haze
  recording. Splitting it out keeps OpenFHE includes out of `libhaze`'s
  translation units and out of downstream consumers.

- **Two replay tiers, one trace format** — The `local` target runs the same
  recorded `.fhetch` trace through the in-process simulator inside `libnbfhetch`
  (no external binary, no transport). The compiler-side targets (`FUNC_SIM`,
  `FHE_SIM`, `FPGA_TRI`, `fhetch_sim`) ship the same trace over HTTP to
  `nbcc_fhetch_replay`. Switching between them is a single `hazeSetTarget`
  call (or `HAZE_TARGET` env var); the application code does not change.

## License

See the `LICENSE` file.
