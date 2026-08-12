# Migrating FIDESlib past the haze hardware-mode removal

`libhaze` no longer models hardware data formats. `hazeReplayConfig` has lost
its `montgomery` and `bit_reversal` fields, and haze now records ordinary-form
FHETCH traces for every replay target.

This is a source-compatibility break for any code that set those fields.
FIDESlib sets them in exactly one place, from a flag no caller ever enables, so
the migration is a deletion — no FIDESlib run changes behaviour.

## Why the fields went away

The niobium-compiler replay driver already performs the Montgomery transform
itself, and decides whether to do so from the replay target's device spec. Three
facts in the compiler tree make haze's copy redundant:

| Where | What it does |
|---|---|
| `fhetch_driver/src/ssa/rules/switchmod_rule.cpp` | `match_switchmod` treats the 4-op form's leading identity `MULI(imm=1, q)` as **optional**, so it recognizes haze's lean 3-op centered-switch chain and rewrites it to `REDC / RAW_ADDI / RAW_MULI(c·R² mod p) / RAW_ADDI(corr·R mod p)`. haze gained nothing by emitting the 4-op shape. |
| `fhetch_driver/src/driver.cpp:230-250` | The driver Montgomery-encodes input limbs on its own: `apply_montgomery = compiler().niobium_hw() && !replay_data_.niobium_hw`. |
| `fhetch_driver/src/driver.cpp:624-629` | Switchmod expansion is gated on `compiler().niobium_hw() || replay_data_.niobium_hw`, and `niobium_hw()` comes from `devices/<id>/spec.yaml`. |

Client recordings were always ordinary-form anyway —
`vendor/niobium-fhetch/src/compiler.cpp` hardcodes `replay["niobium_hw"] = false`
— so the flags only ever selected an instruction *shape* and forwarded
`--niobium_hw`. Both are now the driver's business.

## What changed in the haze ABI

- **Removed:** `hazeReplayConfig::montgomery`, `hazeReplayConfig::bit_reversal`.
  `reduced_noise` stays — it selects the centered FBC variant matching an OpenFHE
  build with `WITH_REDUCED_NOISE`, which is an algorithmic choice, not a data
  format, and is still required for bit-exact parity.
- **Removed:** the `HAZE_ERROR_NOT_SUPPORTED` that a `montgomery`-on-`local`
  configuration used to raise at the first compute. haze no longer has a format
  to refuse.
- **Unchanged:** every function signature, including all four
  `hazeBroadcast*Mrp` entry points. Only the struct shrank.

## Impact on FIDESlib

Three findings from the current `FIDESlib` tree:

1. **`CCParams::SetMontgomery` has no *compiled* call sites.** It is declared
   (`api/CCParams.hpp:63`), defined (`api/CCParams.cpp:135`), and mentioned in two
   doc comments, but no test, example, or benchmark in the build calls it, so
   `params.montgomery` is always `false` at run time. It is, however, taught in
   `HAZE_FPGA_QUICKSTART.md` — including a worked `parameters.SetMontgomery(hardware);`
   — so that document needs rewriting alongside the code (see the edit list).
2. **`hazeBroadcast*Mrp` is never called.** The signature change that isn't (see
   above) is doubly irrelevant here.
3. **FIDESlib already selects hardware mode by target.**
   `api/engine/haze/HazeEngine.cpp:234-236` reads `FIDESLIB_HAZE_TARGET` into
   `hazeReplayConfig::target`, and `jetsun-nid-bench/run_nid_bench.sh:47-48`
   already sets `FPGA_TRI_6_1_0` (an `fpga6.1.0` alias) or `FUNC_SIM`. Since every
   `fpga*` spec sets `montgomery_enabled: true`, the NID benchmark's FPGA runs have
   been getting hardware mode from the device spec all along — never from
   `SetMontgomery`.

So the migration removes dead plumbing. Nothing that runs today changes.

## The edit list

Delete the `montgomery` thread top to bottom. Line numbers are from the tree at
the time of writing; grep for `montgomery` if they have drifted.

| File | Change |
|---|---|
| `api/CCParams.hpp:60-63` | Delete the `SetMontgomery` declaration and its doc comment. |
| `api/CCParams.hpp:78` | Delete the `bool montgomery = false;` member. |
| `api/CCParams.cpp:135-137` | Delete the `SetMontgomery` definition. |
| `api/GenCryptoContext.cpp:29` | `MakeEngine(params.backend, params.reducedNoise)` — drop the third argument. |
| `api/engine/Backend.hpp:23-25` | Drop the `montgomery` parameter from `MakeEngine` and the "and montgomery" clause from its doc comment. |
| `api/engine/Backend.cpp:35, 46, 49` | Drop the parameter, the `HazeEngine` constructor argument, and the `(void)montgomery;` discard in the `#else` branch. |
| `api/engine/haze/HazeEngine.hpp:41-47` | Drop the `montgomery` constructor parameter and its `@param` block. |
| `api/engine/haze/HazeEngine.hpp:516` | Delete the `montgomery_` member. |
| `api/engine/haze/HazeEngine.cpp:214-233` | Delete the `replay.montgomery` / `replay.bit_reversal` assignments and rewrite the surrounding comment: the FBC mode is now `reduced_noise` alone, and the replay target decides the data format. |
| `HAZE_FPGA_QUICKSTART.md:15` | Data-format table row: the selector is now the replay target, not `SetMontgomery`. |
| `HAZE_FPGA_QUICKSTART.md:53-62` | Rewrite the section — it currently states "the format is chosen by the application, not inferred from the target name", which this change inverts. Keep the `SetReducedNoise(true)` advice; it is unaffected. |
| `HAZE_FPGA_QUICKSTART.md:108` | Drop `parameters.SetMontgomery(hardware);` from the example; select hardware via `FIDESLIB_HAZE_TARGET`. |
| `HAZE_FPGA_QUICKSTART.md:245` | Delete the troubleshooting bullet about `SetMontgomery(true)` + `local` failing — that refusal no longer exists. |

Nothing else in FIDESlib references the removed fields.

Note this is an ABI layout change, not only a source change: dropping two `int`
fields moves `reduced_noise` down by 8 bytes, so every consumer must be rebuilt
against the new header — a stale binary would misread the struct silently.

## Getting hardware mode after the change

Set `FIDESLIB_HAZE_TARGET` to a device whose spec declares
`montgomery_enabled: true`. The compiler resolves the string against
`devices/<id>/spec.yaml`, trying the exact directory name, then the lowercased
name, then a scan matching each spec's `hardware.id`
(`src/HardwareSpec.cpp:360-393`) — so `func_sim_hw` and `FUNC_SIM_HW` both work.

| Mode | Targets |
|---|---|
| Hardware (Montgomery) | `func_sim_hw`, every `fpga*` (`fpga1`, `fpga2`, `fpga5.1`–`fpga8.0.1`), `soc1`, `soc2`, `mistic1.0`, `qemu_sim` |
| Ordinary | `func_sim`, `fhe_sim`, `fhetch_sim`, `mistic_perf` |
| In-process simulator | `local` (the default when `FIDESLIB_HAZE_TARGET` is unset) |

`func_sim_hw` is the cheap way to exercise the hardware datapath without an
FPGA: same functional simulator as `FUNC_SIM`, Montgomery enabled.

```sh
# Ordinary-form replay through the functional simulator.
FIDESLIB_BACKEND=haze FIDESLIB_HAZE_TARGET=FUNC_SIM ./your_test

# The same recording replayed in hardware mode.
FIDESLIB_BACKEND=haze FIDESLIB_HAZE_TARGET=func_sim_hw ./your_test
```

Results must match. haze's own suite pins exactly this: `make test-transport` and
`make test-transport-hw` run the same cases against the same oracles.

## One caveat worth knowing: `operand_in_range`

FIDESlib does not call `hazeBroadcast*Mrp` today. If a future port does, this is
the one place where a recording is *not* portable across data formats.

Passing non-zero `operand_in_range` tells haze the operand's coefficients are
small enough that the centered lift is a no-op, so haze elides it and reuses the
operand's residue verbatim under each base prime — saving three recorded ops per
limb.

**That elision is valid for ordinary-form targets only.** A hardware target holds
every value Montgomery-encoded under its own prime, so reusing the word at a
different prime is wrong regardless of how the operand was produced. This includes
a `hazeIsHalfModulus` mask: on hardware a set bit is stored as `R mod p_aux`, not
as a literal `1`, so the elided multiply silently returns garbage in every slot
where the predicate is true.

Leave the flag zero to keep a recording portable; the lift is then always emitted
and correct on every target. There is no safe-operand exemption — if you need the
saving on a hardware target, that is a driver feature request (a cross-ring
re-encode), not something a caller can arrange.

## A second caveat: uploaded data must be evaluation form

This one predates the change described here, but it matters the moment you start
verifying against `func_sim_hw`.

Bytes uploaded with `hazeMemcpy(H2D)` are treated as **evaluation form, natural
coefficient order**. haze records every promoted input that way because the C ABI
carries no format tag. A hardware target's driver bit-reverses inputs on load —
the permutation the datapath expects of evaluation-form data — so uploading
coefficient-form bytes and then calling `hazeNTT` on them produces wrong results
there, while happening to work on an ordinary-form target.

FIDESlib already satisfies this: `HazeEngine.hpp:35` documents its ciphertext
data as "EVALUATION form, natural order, ordinary (non-Montgomery)
representation". The caveat is worth knowing only if a future test or benchmark
hands haze a hand-built coefficient vector.

Note what is *not* affected, so a `func_sim_hw` discrepancy can be triaged
quickly: elementwise operations commute with the load-time permutation, and
symmetric round-trips (`INTT(NTT(x))`, `automorph(k) ∘ automorph(k⁻¹)`) cancel
it. Only a direction-sensitive result checked against a coefficient-domain
oracle can expose it.

## Verifying the migration

```sh
# 1. Build with the haze backend against the updated libhaze.
cmake -B build -DFIDESLIB_ENABLE_HAZE=ON -DFIDESLIB_ENABLE_CUDA=OFF
cmake --build build

# 2. Baseline: the API and parity suites on the in-process simulator.
FIDESLIB_BACKEND=haze ctest --test-dir build --output-on-failure

# 3. Ordinary-form replay through the compiler.
FIDESLIB_BACKEND=haze FIDESLIB_HAZE_TARGET=FUNC_SIM \
  ctest --test-dir build --output-on-failure

# 4. The same suite in hardware mode; results must match step 3.
FIDESLIB_BACKEND=haze FIDESLIB_HAZE_TARGET=func_sim_hw \
  ctest --test-dir build --output-on-failure
```

Steps 3 and 4 need `NBCC_FHETCH_REPLAY` pointing at a built
`nbcc_fhetch_replay`, as the existing transport runs already do.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `no member named 'montgomery' in 'hazeReplayConfig'` (or `bit_reversal`) | The removed fields are still assigned in `HazeEngine.cpp`. | Delete the assignments; select hardware mode with a target instead. |
| `too many arguments to function call` at `MakeEngine` / `HazeEngine(...)` | Only part of the `montgomery` parameter thread was removed. | Work the edit list above end to end — `CCParams` → `GenCryptoContext` → `MakeEngine` → `HazeEngine`. |
| Code expected `HAZE_ERROR_NOT_SUPPORTED` when configuring Montgomery on `local` | That refusal is gone: haze no longer models the format, and a `local` run was always ordinary-form regardless. | Drop the expectation. To get hardware mode, use a hardware target. |
| Hardware mode does not seem to engage | The target does not resolve to a Montgomery device. | Check `montgomery_enabled` in `devices/<id>/spec.yaml` for the id you passed. haze forwards `hazeReplayConfig::target` verbatim and has no say in the format. |
| Wrong results on an FPGA / `func_sim_hw` target only | Most likely a `hazeBroadcast*Mrp` call setting `operand_in_range` on an H2D-derived operand. | See the caveat above; pass `0` to force the lift. |
| Results differ between `FUNC_SIM` and `func_sim_hw` | Not expected — the driver decodes results before they are read. | Reproduce against haze's own `make test-transport` / `make test-transport-hw`; if those agree and FIDESlib does not, the divergence is in the engine layer. |
