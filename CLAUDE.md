# haze — Claude Code working notes

## Hermetic toolchain

All build, test, and lint work for haze must run inside the haze flake's
`devShells.default`:

```sh
cd /path/to/niobium-haze
nix develop
```

The shell provides clang 19, cmake, clang-tools (clang-format, clang-tidy,
clangd), catch2_3, and jujutsu, with `MACOSX_DEPLOYMENT_TARGET=14.0` and a
nix-pinned `SDKROOT`. Do not install build/lint tools globally; if the
shell is missing something, add it to `devShells.default.nativeBuildInputs`
in `flake.nix`.

To run a one-shot command without entering an interactive shell:

```sh
nix develop --command bash -c '<command>'
```

## SDK / ABI mismatch trap on macOS

Symptom: a clean release build links and links cleanly with warnings like
`object file ... was built for newer macOS version (26.0) than being
linked (14.0)`, then `make test-unit-release` segfaults non-deterministically
at code that should be a no-op (e.g. `hazeSetRingDimension(4096)`,
`hazeMalloc`). Probes shift the crash site or mask it; tests pass in
isolation that fail in suite, or vice-versa.

Root cause: OpenFHE (or libnbfhetch built against it) was previously
compiled outside `nix develop` against the host SDK (e.g. macOS 26.0)
while haze itself is compiled inside the devshell against the pinned SDK
(macOS 14.0). The two libc++ ABI versions collide in the same process.
Memory-layout-sensitive heisenbugs follow.

Fix: rebuild OpenFHE and the niobium-fhetch chain inside the devshell so
every artifact in the link graph has the same `LC_BUILD_VERSION minos`.
The cheap path:

```sh
# Wipe haze-side build dirs (preserves the existing OpenFHE install in
# vendor/niobium-fhetch/vendor/lib/openfhe).
rm -rf build dbuild
nix develop --command bash -c 'EXTERNAL_OPENFHE=1 make build-release'
```

If the warnings persist, also wipe and rebuild OpenFHE itself:

```sh
rm -rf vendor/niobium-fhetch/vendor/openfhe/build \
       vendor/niobium-fhetch/vendor/openfhe/dbuild \
       vendor/niobium-fhetch/vendor/lib/openfhe
nix develop --command bash -c 'make build-openfhe-release && make build-release'
```

Verify the fix with `otool -l <dylib> | grep -A3 LC_BUILD_VERSION` —
`minos` should be `14.0` for libhaze.dylib, libnbfhetch.dylib, and every
libOPENFHE*.dylib under `vendor/niobium-fhetch/vendor/lib/openfhe/lib`.

Recognize the symptom early: any non-trivial test segfault accompanied
by `built for newer macOS version` linker warnings is this trap until
proven otherwise. Do not waste cycles on probe-based bisection before
checking the SDK/ABI alignment.

## Baselines (post-fix)

- `make test-unit-release` → **53/53 pass**, ~1s.
- `make test-integration-release NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler`
  → **18/27 pass**. The 9 failures in `test_basis_convert.cpp` at lines
  117/245/291/331/381/443/757(x3) are multi-residue (`hazeModUp` /
  `hazeModDown`) cases blocked on bridge MRP support; pre-existing,
  out of scope for the haze RYANPR plan. A passing integration run still
  exits non-zero (Catch2 reports the failures as test failures).

## Out of scope

Nothing under `vendor/` is editable in the RYANPR resolution work — the
companion `niobium-fhetch` review is a separate PR. Reading public
headers via the `Niobium::fhetch` CMake target is fine; modifying source
under `vendor/` is not.
