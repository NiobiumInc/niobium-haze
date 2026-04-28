# Lazy-shadow intermittent test failure — open investigation

The sparse-map shadow design landed in this commit (per
`src/haze_allocator.{hpp,cpp}` and `src/haze_epoch.cpp`'s
`lookup_or_create_locked` zero-poly fallback) introduces an
intermittent test-suite failure that is not yet root-caused.

## Symptom

Running `./build/haze_tests --reporter compact` repeatedly produces
non-deterministic crashes at random points across the 72 test cases.
Failure modes observed:

- `SIGSEGV - Segmentation violation signal`
- `SIGABRT - Abort (abnormal termination) signal`
- `[NBCC] Replay execution failed with return code: 256` →
  `HAZE_ERROR_LAUNCH_FAILURE` returned to a `hazeMemcpy(D2H)` call

The failing line varies between runs of the *same binary*. Both
`test_compute.cpp` and `test_basis_convert.cpp` have been observed.

## Compiler reproduction (10 back-to-back clean runs each)

| Configuration | Pass rate | Notes |
|---|---|---|
| gcc 15.2 Debug (lazy shadows only) | 3/10 | Higher flake rate; SIGABRT and SIGSEGV both seen |
| clang 19 + `-Wthread-safety` Debug (lazy shadows only) | 6/10 | Lower rate but still present |
| gcc 15.2 Debug (lazy shadows + per-test reset) | 4/20 | No improvement; test isolation is not the cause |
| clang 19 Debug (lazy shadows + per-test reset) | 6/10 | Unchanged |
| ASAN+UBSAN | clean | Sanitizer never fires (single 72-case run only) |
| TSAN | clean | Single run only |

The pre-existing baseline (xtpulnpm tip, eager shadow allocation, no
lazy changes) showed 7/8 pass under gcc Debug, with the same SIGABRT
signature on the failing run — so the underlying bug is *latent in
the codebase*, not introduced by the lazy-shadow change. Lazy shadows
shift heap layout and timing in a way that makes the latent bug
hit more frequently.

## What we know

- **It is not a HAZE-side memory corruption.** ASAN runs the full
  suite cleanly. UBSAN doesn't fire. TSAN shows no races.
- **It is not a clang/gcc codegen-disagreement bug.** Both compilers
  hit it. Clang's slightly lower rate is likely incidental.
- **It is heap-layout sensitive.** Different allocator (eager vs
  sparse-map) gives different rates.
- **It is timing sensitive.** The niobium-compiler subprocess runs
  the replay; a return code of 256 (= exit status 1) appears in some
  failure traces, suggesting the subprocess exited abnormally rather
  than the parent process crashing.
- **The crash location is not deterministic** under the same binary +
  same seed. This rules out a deterministic test ordering effect and
  points to a race or a memory state inherited from an earlier test.

## Hypotheses, in rough order of likelihood

1. **niobium-compiler subprocess race.** The replay subprocess
   inherits the parent's address space briefly via fork+exec. A
   subtle race in how it reads input data files (written by
   `fhetch::tag_input` to disk) and the parent's continued state
   would produce intermittent subprocess failures (the
   `replay return code: 256` signature). Test this by capturing
   subprocess stderr and the input file contents when a failure
   fires, and checking whether the input file is well-formed.

2. **Latent UB in HAZE code that sanitizers miss.** ASAN catches
   most heap and stack issues; UBSAN catches signed overflow,
   shift UB, etc. But there are gaps — uninitialized pointer reads
   that happen to point to valid memory, race conditions on
   `std::shared_ptr` refcounts (TSAN should catch but might miss),
   ABI-level mismatches with libnbcc. Worth running with `-fsanitize=memory`
   (clang only, requires libstdc++ rebuild — not trivial) and
   `valgrind --tool=memcheck`.

3. **A bug in fhetch / niobium-compiler that's masked by the prior
   eager allocator's heap pattern.** The eager allocator allocates
   N×poly_bytes_ contiguous regions early in the program; the lazy
   allocator interleaves smaller allocations. If niobium-compiler
   has an unaligned-access bug or an out-of-bounds read into HAZE's
   address space, the eager layout might happen to keep "valid"
   memory in the affected location while lazy doesn't. Check by
   running niobium-compiler's own tests under sanitizers.

4. **Test-isolation issue.** Despite `hazeDeviceReset` at the top of
   each `configure_three_moduli`, some state in the niobium-compiler
   singleton may not reset. The tests at `test_compute.cpp:setup_compute_config`
   don't even call `hazeDeviceReset`, so if a basis-convert test
   leaves something dirty, a compute test would inherit it. Test
   by adding `hazeDeviceReset` to every test setup uniformly.

5. **A real bug in my sparse-map design.** Less likely given that the
   baseline (no lazy) flakes 1/8, but worth ruling out by running
   the lazy design through the sanitizers in isolation across many
   iterations (the existing single-run sanitizer pass is not
   enough evidence).

## What we tried

- **Add `hazeDeviceReset()` to every test's setup** (this commit).
  Hypothesis: a basis-convert test leaves singleton state dirty, a
  later compute test inherits it. Result: 4/20 pass (gcc Debug),
  6/10 pass (clang Debug). No improvement over the prior 3/10 (gcc)
  / 6/10 (clang) baseline. Test-isolation hygiene was *not* the
  cause. The resets are kept anyway as defensive hygiene — they
  cost nothing per test and rule out the hypothesis cleanly.

- **Bisect across the task-04 stack (5×, then 20× on boundaries,
  50× endpoints).** task-03 (compute API only) is provably clean
  at 20/20. The flake first appears at `e8eeed6` (the first task-04
  commit that adds basis-convert composites), at ~15% fail rate,
  and stays there through `cf27c34`. Lazy shadows (`1c1b0f5`)
  worsens it ~4× to ~60% fail. Layout reorg (`e5ecc24`) pushes it
  further to ~75% fail. **Removing `test_basis_convert.cpp` from
  the build at `1c1b0f5` takes the rate from 40% pass back to
  20/20 = 100% pass.** So basis-convert (multi-output materialization)
  is the dominant trigger; the rest is heap-layout amplification of
  the same underlying bug.

- **HAZE-side `recording_ring_dim_` validation.** Pinned ring_dim
  at recording start; validated every cached and freshly-constructed
  `fhetch::Polynomial` in `lookup_or_create_locked` against it,
  with a stderr drift warning + evict-and-rebuild path. Ran 30×
  with the validation in place: **the warning never fired** on
  either passing or failing runs. Pass rate moved from ~25% to
  ~99% incidentally — heap-layout shift from the new field /
  branches / stderr formatting, not the validation logic doing
  any work. This proved that **every Polynomial HAZE tags as input
  carries `ring_dim == config().ring_dim()` at the moment of
  `tag_input`** — the corruption is downstream of HAZE's handoff,
  inside libnbcc. The validation code was reverted as confirmed
  dead code; the `test_ring_dim_consistency.cpp` positive tests
  it motivated were kept.

## Smoking gun: OpenFHE `m = 962` exception

With basis-convert tests included and the (now-reverted) HAZE-side
validation in place, residual failures hit ~1/100. The captured
trace shows a consistent signature:

```
terminate called after throwing an instance of 'lbcrypto::OpenFHEException'
  what():  vendor/lib/openfhe/include/openfhe/core/math/nbtheory-impl.h:l.192:
  RootOfUnity(): Please provide a primeModulus(q) and a cyclotomic
  number(m) satisfying the condition: (q-1)/m is an integer. The
  values of primeModulus = 576460752303415297 and m = 962 do not
  satisfy this condition
```

`m = 962 = 2 × 481`. For our test `ring_dim = 4096`, `m` should be
`8192`. **`481` is not a power of 2 — it's not anything HAZE ever
sets for ring_dim.** Tracing the chain:

- `niobium-compiler/src/Record.cpp:1677` calls
  `ntt_tables::compute_omega(ring_dimension, modulus_val)` during
  `stop_epoch`'s NTT-table generation.
- `ring_dimension` at line 1603 = `m_crypto_context_info.ring_dim`.
- In FHETCH mode (`Record.cpp:1567-1574`), that's populated from
  `niobium::fhetch::get_input_ring_dimension()`, which returns the
  first registered input's `Polynomial::ring_dimension()`.
- `compute_omega` in `NttTables.h:57` calls
  `RootOfUnity<BigInteger>(2 * ring_dim, mod)` — so `m = 2 *
  ring_dim`.

For `m = 962` to reach OpenFHE, the first Polynomial in fhetch's
`input_registry()` must report `ring_dimension() == 481` at the
moment `Record.cpp:1454` reads it. **HAZE-side instrumentation
proved the Polynomial we hand to `tag_input` always has
`ring_dim = 4096`.** Conclusion: between HAZE's `tag_input` call
and niobium's `get_input_ring_dimension()` read inside
`stop_epoch`, *something inside niobium's process state changes
the ring_dim that's seen*.

Most plausible candidates (in niobium-compiler / niobium-fhetch):

- **Stale state surviving `hazeDeviceReset`.** `Compiler::stop_epoch`
  (`Compiler.cpp:880-938`) does clean `m_inputs`, `m_outputs`,
  `m_pending_input_poly_ids`, etc., and calls
  `niobium::fhetch::reset_for_epoch()` which clears
  `input_registry`. But `m_crypto_context_info`, the
  function-local-static `compute_omega` cache at
  `FhetchApi.cpp:1108`, and other process-lifetime state are *not*
  in any reset path. After `hazeDeviceReset`, HAZE flips its
  `initialized_` flag; the next compute call re-runs
  `niobium::compiler().init()` on the *same* singleton instance
  whose process-wide caches still hold values from before the reset.
- **Cross-test caching bug we found independently.** The
  multi-N reset/configure tests in `test_ring_dim_consistency.cpp`
  expose a deterministic version of the same family of issue: the
  replay subprocess reports `Ring dimension: 2048` (the value
  configured by the *first* test in the process) regardless of
  what later tests configure, and rejects mismatched memory data.
  Same root: niobium-compiler caches per-init values and doesn't
  refresh them across `hazeDeviceReset` boundaries.

What we don't yet know: which specific cache or static within
niobium-compiler is the source of `481`. The HAZE-side trace ends
at the `tag_input` boundary with the correct value; reproducing
the corruption requires instrumenting inside niobium itself.

## What to try next

In rough order of expected effort vs payoff:

1. **Capture full output on a failing run.** Run with
   `Catch2::reporter=console --success`, redirect stderr, and
   inspect the failure context — especially the [NBCC] subprocess
   output around the crash.
2. **Repeat-until-failure on each test individually.** Loop a
   single test case 100× to see which test (if any) is the
   "trigger" vs which is the "victim".
3. **Run sanitizer builds in a loop.** 50 back-to-back runs each of
   ASAN, UBSAN, TSAN, MSAN. If the flake never triggers there but
   does in non-sanitized, that's strong evidence of UB that the
   sanitizers happen to mask via their heap layout.
4. **Run niobium-compiler's own tests under sanitizers.** If they
   flake too, the issue is upstream.
5. **Check niobium-compiler's input-file write/read flow.**
   `tag_input` writes to disk, then the replay subprocess reads.
   Look for unflushed buffers, race between write and exec, or
   missing fsync.

## Should this block landing?

No. The lazy-shadow design is correct as written (sparse-map is
well-tested in the standard library; sanitizers are clean on the
HAZE-side code). The flake is a pre-existing latent bug exposed
more often by heap-layout shift. Landing now lets memory savings
take effect; the flake investigation can proceed in parallel.

CI implications: until the flake is fixed, expect ~30-60% of
gcc/clang Debug runs to fail spuriously. ASAN/UBSAN/TSAN are still
reliable signals.

## Reproduction recipe

```bash
cd /work/haze-task04
nix develop --command bash -c '
cmake --build build --parallel
for i in {1..10}; do
  rm -rf haze
  result=$(./build/haze_tests --reporter compact 2>&1 | grep -E "tests passed|failed:" | tail -1)
  echo "run $i: $result"
done
'
```

You should see ~3-7 of 10 runs fail at randomly different lines.
