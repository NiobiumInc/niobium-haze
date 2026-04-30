# FHETCH Polynomial IR Reference

The FHETCH Polynomial IR is a library-independent intermediate representation for Fully Homomorphic Encryption programs, following the [FHETCH specification](https://fhetch.org). It expresses FHE computations as polynomial-level operations that map directly to the Niobium BASALISC hardware accelerator ISA.

This document is a comprehensive reference for the IR's data types, instructions, gadgets, and supporting infrastructure as implemented in the Niobium compiler.

## Source locations

| Component                         | Path                                |
| --------------------------------- | ----------------------------------- |
| Public API header                 | `include/niobium/fhetch_api.h`      |
| Implementation                    | `src/FhetchApi.cpp`                 |
| Existing documentation            | `docs/fhetch/FHETCH.md`             |
| FHETCH driver (trace interpreter) | `fhetch_driver/`                    |
| FHETCH driver documentation       | `docs/fhetch/FHETCH_DRIVER.md`      |
| FHETCH_SIM device spec            | `devices/fhetch_sim/spec.yaml`      |
| Simple example                    | `examples/fhetch/simple_fhetch.cpp` |
| Epoch example                     | `examples/fhetch/epoch_fhetch.cpp`  |
| Simple example test               | `tests/fhetch/fhetch_test.py`       |
| Epoch example test                | `tests/fhetch/epoch_fhetch_test.py` |
| HEIR integration design           | `docs/heir/HEIR_FHETCH.md`          |

All FHETCH symbols live in the `niobium::fhetch` namespace.

## Enumerations

| Enum         | Values                      | Description                                                                          |
| ------------ | --------------------------- | ------------------------------------------------------------------------------------ |
| `NumberType` | `Integer`, `NonInteger`     | Whether components are modular integers or fixed-point/floating-point values.        |
| `Format`     | `Coefficient`, `Evaluation` | Polynomial representation domain: time-domain coefficients vs. NTT/FFT point values. |

## Data types

All types use an opaque pimpl pattern (`shared_ptr` to an internal `*Impl` struct). Each polynomial is assigned a synthetic address from a monotonically increasing allocator, starting at 0.

### Polynomial (SRP -- single-residue polynomial)

A vector of N components under a single modulus.

| Constructor / Factory   | Signature                                                                                                | Description                                |
| ----------------------- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------ |
| Default                 | `Polynomial()`                                                                                           | Empty/invalid polynomial.                  |
| Zero (integer)          | `Polynomial::zeros(uint64_t ring_dim, Format fmt = Evaluation)`                                          | N zero-valued integer components.          |
| Zero (non-integer)      | `Polynomial::zeros_ni(uint64_t ring_dim, Format fmt = Evaluation)`                                       | N zero-valued floating-point components.   |
| From data (integer)     | `Polynomial::from_data(const vector<uint64_t>& components, uint64_t ring_dim, Format fmt = Evaluation)`  | Integer polynomial from raw values.        |
| From data (non-integer) | `Polynomial::from_data_ni(const vector<double>& components, uint64_t ring_dim, Format fmt = Evaluation)` | Floating-point polynomial from raw values. |

| Accessor           | Return type  | Description                              |
| ------------------ | ------------ | ---------------------------------------- |
| `ring_dimension()` | `uint64_t`   | Ring dimension N.                        |
| `number_type()`    | `NumberType` | Integer or NonInteger.                   |
| `format()`         | `Format`     | Coefficient or Evaluation.               |
| `is_valid()`       | `bool`       | True if the polynomial holds valid data. |

### Scalar

A single value, integer or floating-point.

| Factory      | Signature                           | Description         |
| ------------ | ----------------------------------- | ------------------- |
| From integer | `Scalar::from_int(uint64_t value)`  | Integer scalar.     |
| From double  | `Scalar::from_double(double value)` | Non-integer scalar. |

| Accessor        | Return type  | Description                          |
| --------------- | ------------ | ------------------------------------ |
| `number_type()` | `NumberType` | Integer or NonInteger.               |
| `is_valid()`    | `bool`       | True if the scalar holds valid data. |

### MRP (multi-residue polynomial)

A set of polynomials indexed by modulus, representing an RNS decomposition. Keyed by modulus value; cannot hold duplicate moduli.

| Constructor / Factory | Signature                                                          | Description                                   |
| --------------------- | ------------------------------------------------------------------ | --------------------------------------------- |
| Default               | `MRP()`                                                            | Empty MRP with empty base.                    |
| From base             | `MRP(const ModuliBase& base, uint64_t ring_dim = 0)`               | Zero-initialized, one polynomial per modulus. |
| From pairs            | `MRP::from_pairs(const vector<pair<Polynomial, uint64_t>>& pairs)` | Explicit (polynomial, modulus) pairs.         |

| Accessor                 | Return type         | Description                                               |
| ------------------------ | ------------------- | --------------------------------------------------------- |
| `base()`                 | `const ModuliBase&` | Ordered set of prime moduli.                              |
| `num_residues()`         | `size_t`            | Number of residues (== `base().size()`).                  |
| `operator[](uint64_t q)` | `Polynomial&`       | Polynomial for modulus q. Throws if q is not in the base. |
| `is_valid()`             | `bool`              | True if the MRP holds valid data.                         |

### MRS (multi-residue scalar)

A set of scalars indexed by modulus, analogous to MRP.

| Constructor / Factory | Signature                                                      | Description                       |
| --------------------- | -------------------------------------------------------------- | --------------------------------- |
| Default               | `MRS()`                                                        | Empty MRS.                        |
| From base             | `MRS(const ModuliBase& base)`                                  | Zero-initialized scalars.         |
| From pairs            | `MRS::from_pairs(const vector<pair<Scalar, uint64_t>>& pairs)` | Explicit (scalar, modulus) pairs. |

| Accessor                 | Return type         | Description                       |
| ------------------------ | ------------------- | --------------------------------- |
| `base()`                 | `const ModuliBase&` | Ordered set of prime moduli.      |
| `num_residues()`         | `size_t`            | Number of residues.               |
| `operator[](uint64_t q)` | `Scalar&`           | Scalar for modulus q.             |
| `is_valid()`             | `bool`              | True if the MRS holds valid data. |

### SRPArray

Ordered, position-indexed array of single-residue polynomials. Unlike MRP, can hold polynomials with duplicate moduli.

| Constructor      | Signature                                      | Description                        |
| ---------------- | ---------------------------------------------- | ---------------------------------- |
| Default          | `SRPArray()`                                   | Empty array (length 0).            |
| Sized            | `SRPArray(size_t n)`                           | n default-initialized polynomials. |
| Initializer list | `SRPArray(initializer_list<Polynomial> polys)` | From a list of polynomials.        |

| Method                        | Return type   | Description                                                 |
| ----------------------------- | ------------- | ----------------------------------------------------------- |
| `length()`                    | `size_t`      | Number of elements.                                         |
| `operator[](size_t i)`        | `Polynomial&` | Element at index i. Throws `out_of_range` if i >= length(). |
| `append(const Polynomial& p)` | `void`        | Append a polynomial.                                        |
| `is_valid()`                  | `bool`        | True if the array holds valid data.                         |

### MRPArray

Ordered array of MRPs. Each element may have a different base and number of residues.

| Constructor      | Signature                              | Description                 |
| ---------------- | -------------------------------------- | --------------------------- |
| Default          | `MRPArray()`                           | Empty array (length 0).     |
| Sized            | `MRPArray(size_t n)`                   | n default-initialized MRPs. |
| Initializer list | `MRPArray(initializer_list<MRP> mrps)` | From a list of MRPs.        |

| Method                 | Return type | Description                                                 |
| ---------------------- | ----------- | ----------------------------------------------------------- |
| `length()`             | `size_t`    | Number of elements.                                         |
| `operator[](size_t i)` | `MRP&`      | Element at index i. Throws `out_of_range` if i >= length(). |
| `append(const MRP& m)` | `void`      | Append an MRP.                                              |
| `is_valid()`           | `bool`      | True if the array holds valid data.                         |

### Type aliases

| Alias        | Definition              | Description                                          |
| ------------ | ----------------------- | ---------------------------------------------------- |
| `ModuliBase` | `std::vector<uint64_t>` | An ordered set of prime moduli defining an RNS base. |

### Structs

| Struct                 | Fields                                                                                                                               | Description                                                   |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------- |
| `HardwareCapabilities` | `supported_ring_dimensions`, `max_modulus_bits` (63), `max_modulus_chain_length` (64), `supported_gadgets`, `supported_optional_ops` | Capabilities advertised by a hardware target to the compiler. |
| `ProgramParameters`    | `ring_dimension`, `rns_primes`, `representation` (Evaluation), `coefficient_precision_bits` (64), `prime_congruence`                 | Parameter choices flowing from the compiler to the hardware.  |

## Baseline instructions

These map directly to the Niobium hardware ISA. Every compliant hardware target must implement them. Integer variants operate modulo a prime q; non-integer variants (in the optional section) use q=0 internally.

| FHETCH function  | Signature                                                                                                 | ISA opcode      | Semantics                                                                                                                                                                     |
| ---------------- | --------------------------------------------------------------------------------------------------------- | --------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `sr_addp`        | `(const Polynomial& a, const Polynomial& b, uint64_t q) -> Polynomial`                                    | ADD             | f_i = (a_i + b_i) mod q                                                                                                                                                       |
| `sr_subp`        | `(const Polynomial& a, const Polynomial& b, uint64_t q) -> Polynomial`                                    | SUB             | f_i = (a_i - b_i) mod q                                                                                                                                                       |
| `sr_mulp`        | `(const Polynomial& a, const Polynomial& b, uint64_t q) -> Polynomial`                                    | MUL             | f_i = (a_i \* b_i) mod q                                                                                                                                                      |
| `sr_addps`       | `(const Polynomial& a, const Scalar& s, uint64_t q) -> Polynomial`                                        | ADDI            | f_i = (a_i + s) mod q (evaluation mode)                                                                                                                                       |
| `sr_addps_coeff` | `(const Polynomial& a, const Scalar& s, uint64_t q) -> Polynomial`                                        | ADDI            | f*0 = (a_0 + s) mod q, f*{i>0} = a_i (coefficient mode)                                                                                                                       |
| `sr_subps`       | `(const Polynomial& a, const Scalar& s, uint64_t q) -> Polynomial`                                        | SUBI            | f_i = (a_i - s) mod q (evaluation mode)                                                                                                                                       |
| `sr_subps_coeff` | `(const Polynomial& a, const Scalar& s, uint64_t q) -> Polynomial`                                        | SUBI            | f*0 = (a_0 - s) mod q, f*{i>0} = a_i (coefficient mode)                                                                                                                       |
| `sr_mulps`       | `(const Polynomial& a, const Scalar& s, uint64_t q) -> Polynomial`                                        | MULI            | f_i = (a_i \* s) mod q                                                                                                                                                        |
| `sr_negp`        | `(const Polynomial& a, uint64_t q) -> Polynomial`                                                         | SUB             | f_i = (-a_i) mod q. Emitted as `sub(result, zero, a, q)`, allocating a zero polynomial first.                                                                                 |
| `sr_ntt`         | `(const Polynomial& a, uint64_t q) -> Polynomial`                                                         | NTT1 + NTT2     | Forward negacyclic NTT. Computes the primitive 2N-th root of unity (omega) from q and N via NTL; cached per modulus. Converts coefficient -> evaluation format.               |
| `sr_intt`        | `(const Polynomial& a, uint64_t q) -> Polynomial`                                                         | INTT1 + INTT2   | Inverse negacyclic NTT. Uses the same cached omega. Converts evaluation -> coefficient format.                                                                                |
| `sr_permute`     | `(const Polynomial& a, const vector<uint64_t>& srcs, const vector<int>& signs, uint64_t q) -> Polynomial` | MORPH1 + MORPH2 | General permutation with sign flips. f_i = signs[i] \* a[srcs[i]] (mod q when negative). Currently emitted as a single morph with rot=0; full srcs/signs encoding is pending. |
| `halt`           | `() -> void`                                                                                              | STOP            | Signal the hardware to stop and notify the host.                                                                                                                              |

## Optional operations

These are not required by all hardware targets. They extend the IR for scheme-specific needs.

### Non-integer arithmetic

These mirror the baseline instructions but operate on floating-point polynomial components without a modulus. Internally they emit the same generator opcodes (ADD, SUB, MUL, ADDI, SUBI, MULI) with mod=0 to signal non-integer mode.

| FHETCH function     | Signature                                                  | Semantics                    |
| ------------------- | ---------------------------------------------------------- | ---------------------------- |
| `sr_addp_ni`        | `(const Polynomial& a, const Polynomial& b) -> Polynomial` | f_i = a_i + b_i              |
| `sr_subp_ni`        | `(const Polynomial& a, const Polynomial& b) -> Polynomial` | f_i = a_i - b_i              |
| `sr_mulp_ni`        | `(const Polynomial& a, const Polynomial& b) -> Polynomial` | f_i = a_i \* b_i             |
| `sr_negp_ni`        | `(const Polynomial& a) -> Polynomial`                      | f_i = -a_i                   |
| `sr_addps_ni`       | `(const Polynomial& a, const Scalar& s) -> Polynomial`     | f_i = a_i + s                |
| `sr_addps_coeff_ni` | `(const Polynomial& a, const Scalar& s) -> Polynomial`     | f*0 = a_0 + s, f*{i>0} = a_i |
| `sr_subps_ni`       | `(const Polynomial& a, const Scalar& s) -> Polynomial`     | f_i = a_i - s                |
| `sr_subps_coeff_ni` | `(const Polynomial& a, const Scalar& s) -> Polynomial`     | f*0 = a_0 - s, f*{i>0} = a_i |
| `sr_mulps_ni`       | `(const Polynomial& a, const Scalar& s) -> Polynomial`     | f_i = a_i \* s               |

### Fourier transforms

Alternative to NTT/INTT for hardware supporting FHEW/TFHE schemes. Internally emitted as NTT/INTT with mod=0 and omega=0.

| FHETCH function | Signature                             | Semantics                                                                 |
| --------------- | ------------------------------------- | ------------------------------------------------------------------------- |
| `sr_ft`         | `(const Polynomial& a) -> Polynomial` | Negacyclic Fourier Transform (complex-valued). Coefficient -> Evaluation. |
| `sr_ift`        | `(const Polynomial& a) -> Polynomial` | Inverse Negacyclic Fourier Transform. Evaluation -> Coefficient.          |

### Coefficient access

| FHETCH function    | Signature                                                            | Status              | Semantics                                            |
| ------------------ | -------------------------------------------------------------------- | ------------------- | ---------------------------------------------------- |
| `sr_coeff_extract` | `(const Polynomial& p, uint64_t i) -> Scalar`                        | Stub (returns 0)    | Extract the i-th component as a scalar.              |
| `sr_coeff_assign`  | `(const Polynomial& p, uint64_t i, const Scalar& val) -> Polynomial` | Stub (returns copy) | Return a new polynomial with component i set to val. |

### Torus and sample operations (TFHE/FHEW)

| FHETCH function       | Signature                                                      | Status | Semantics                                                                                       |
| --------------------- | -------------------------------------------------------------- | ------ | ----------------------------------------------------------------------------------------------- |
| `sr_torus_mod_reduce` | `(const Polynomial& p, double c) -> Polynomial`                | Stub   | Torus modular reduction: coefficients in [c-0.5, c+0.5) equal to p up to an integer polynomial. |
| `sr_sample_extract`   | `(const SRPArray& rlwe, uint64_t lwe_dim) -> vector<uint64_t>` | Stub   | Sample extraction from RLWE (length-2 SRPArray) to LWE vector.                                  |

## Gadgets -- polynomial level

Class I gadgets are simple compositions of baseline/optional instructions, verifiable by inspection.

### Automorphisms

All automorphisms emit MORPH instructions.

| FHETCH function          | Signature                                                          | Description                                                                                                                          |
| ------------------------ | ------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------ |
| `sr_automorph_eval`      | `(const Polynomial& x, uint64_t k) -> Polynomial`                  | Galois automorphism X -> X^k in evaluation representation. k is an odd integer in [1, 2N-1]. Uses COPY_MODULUS (0xFFFFFFFFFFFFFFFF). |
| `sr_automorph_coeff`     | `(const Polynomial& x, uint64_t k, uint64_t q) -> Polynomial`      | Galois automorphism in coefficient representation.                                                                                   |
| `sr_rot_automorph_coeff` | `(const Polynomial& x, uint64_t offset, uint64_t q) -> Polynomial` | Negacyclic rotation automorphism in coefficient representation. Offset in [0, N-1].                                                  |

### Batch transforms

| FHETCH function | Signature                         | Description                     |
| --------------- | --------------------------------- | ------------------------------- |
| `sr_batch_ft`   | `(const SRPArray& x) -> SRPArray` | Apply `sr_ft` to each element.  |
| `sr_batch_ift`  | `(const SRPArray& x) -> SRPArray` | Apply `sr_ift` to each element. |

## Gadgets -- multi-residue arithmetic

These expand into per-residue baseline instructions, iterating over every modulus in the shared base.

| FHETCH function | Signature                                                   | Expansion                                                  |
| --------------- | ----------------------------------------------------------- | ---------------------------------------------------------- |
| `mr_addp`       | `(const MRP& x, const MRP& y) -> MRP`                       | For each q in base: z[q] = `sr_addp`(x[q], y[q], q)        |
| `mr_subp`       | `(const MRP& x, const MRP& y) -> MRP`                       | For each q in base: z[q] = `sr_subp`(x[q], y[q], q)        |
| `mr_mulp`       | `(const MRP& x, const MRP& y) -> MRP`                       | For each q in base: z[q] = `sr_mulp`(x[q], y[q], q)        |
| `mr_mulps`      | `(const MRP& x, const MRS& s) -> MRP`                       | For each q in base: z[q] = `sr_mulps`(x[q], s[q], q)       |
| `mr_addps`      | `(const MRP& x, const MRS& s) -> MRP`                       | For each q in base: z[q] = `sr_addps`(x[q], s[q], q)       |
| `mr_ntt`        | `(const MRP& x) -> MRP`                                     | For each q in base: z[q] = `sr_ntt`(x[q], q)               |
| `mr_intt`       | `(const MRP& x) -> MRP`                                     | For each q in base: z[q] = `sr_intt`(x[q], q)              |
| `mr_zeros`      | `(const ModuliBase& target_base, uint64_t ring_dim) -> MRP` | Construct a zero-initialized MRP. No instructions emitted. |

## Gadgets -- MRP residue manipulation

These are structural operations that create new MRP objects by rearranging residues. No hardware instructions are emitted.

| FHETCH function | Signature                                                  | Description                                                                   |
| --------------- | ---------------------------------------------------------- | ----------------------------------------------------------------------------- |
| `mr_append_srp` | `(const MRP& x, const Polynomial& a, uint64_t q_a) -> MRP` | New MRP with base = x.base() union {q_a}.                                     |
| `mr_union`      | `(const MRP& x, const MRP& y) -> MRP`                      | Merge two MRPs with mutually exclusive bases. base = x.base() union y.base(). |
| `mr_subset`     | `(const MRP& x, const ModuliBase& subbase) -> MRP`         | Restrict MRP to a sub-base. subbase must be a subset of x.base().             |

## Gadgets -- base conversion and rescaling

| FHETCH function     | Signature                                               | Description                                                                                                                                                                                               |
| ------------------- | ------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `fast_base_convert` | `(const MRP& x, const ModuliBase& target_base) -> MRP`  | CRT-based approximate base conversion from x's current base to target_base. Emits per-residue `sr_mulps` and `sr_addp` instructions. Input must be in coefficient mode.                                   |
| `rescale_fbc`       | `(const MRP& x, const ModuliBase& rescale_base) -> MRP` | CKKS rescale: removes residues in rescale_base and rescales. Returns MRP in base = x.base() \ rescale_base. Calls `fast_base_convert` internally, then emits `sr_subp` and `sr_mulps` per output residue. |

## Gadgets -- MRP array operations

| FHETCH function   | Signature                                       | Description                                                                                                                           |
| ----------------- | ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `mrpa_dotproduct` | `(const MRPArray& x, const MRPArray& y) -> MRP` | z = sum_i mr_mulp(x[i], y[i]). Both arrays must have the same length and all MRPs must share the same base. Key-switching inner loop. |

## Gadgets -- decomposition

| FHETCH function      | Signature                                                                                     | Description                                                                                                                                                              |
| -------------------- | --------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `dig_decomp`         | `(const MRP& x, const vector<ModuliBase>& digit_bases, const ModuliBase& p_base) -> MRPArray` | CKKS/BGV/BFV hybrid key-switching digit decomposition. Returns MRPArray of length d where element i is base-extended from digit_bases[i] to (x.base() union p_base).     |
| `gadget_decomp`      | `(const Polynomial& x, uint64_t base, uint64_t n_levels) -> SRPArray`                         | TFHE gadget decomposition (unsigned integer). Returns SRPArray of length n_levels. Stub -- requires integer division/bit-shift instructions not yet in the baseline ISA. |
| `gadget_decomp_pow2` | `(const Polynomial& x, uint64_t log_base, uint64_t n_levels) -> SRPArray`                     | TFHE gadget decomposition for power-of-two base. Same status as `gadget_decomp`.                                                                                         |

## Gadgets -- composite operations

| FHETCH function     | Signature                                                                               | Description                                                                                                                                                                                                                  |
| ------------------- | --------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `gsw_rlwe_ext_prod` | `(const SRPArray& gsw, const SRPArray& rlwe_in, uint64_t l, uint64_t base) -> SRPArray` | GSW/RLWE external product. gsw is a length-4l SRPArray, rlwe_in is length 2. Internally calls `gadget_decomp`, `sr_batch_ft`, non-integer `sr_mulp_ni`/`sr_addp_ni`, and `sr_batch_ift`. Returns a length-2 RLWE ciphertext. |
| `ckks_bootstrap`    | `(const MRPArray& ct_in, const MRPArray& aux_data) -> MRPArray`                         | CKKS bootstrapping. Stub -- returns input unchanged. Requires scheme-specific parameter flow from hardware capabilities.                                                                                                     |

## Input/output tagging

Polynomials must be tagged as inputs or outputs so the optimizer's observability analysis knows which addresses are live. Without tags, all instructions are eliminated as dead code.

| Function     | Signature                                           | Description                                         |
| ------------ | --------------------------------------------------- | --------------------------------------------------- |
| `tag_input`  | `(const string& name, const Polynomial& p) -> void` | Mark a single polynomial as a named input.          |
| `tag_input`  | `(const string& name, const MRP& m) -> void`        | Mark all residues of an MRP as named inputs.        |
| `tag_input`  | `(const string& name, const SRPArray& arr) -> void` | Mark all elements of an SRP array as named inputs.  |
| `tag_input`  | `(const string& name, const MRPArray& arr) -> void` | Mark all elements of an MRP array as named inputs.  |
| `tag_output` | `(const string& name, const Polynomial& p) -> void` | Mark a single polynomial as a named output (probe). |
| `tag_output` | `(const string& name, const MRP& m) -> void`        | Mark all residues of an MRP as named outputs.       |
| `tag_output` | `(const string& name, const SRPArray& arr) -> void` | Mark all elements of an SRP array as named outputs. |
| `tag_output` | `(const string& name, const MRPArray& arr) -> void` | Mark all elements of an MRP array as named outputs. |

Names are used as keys in the serialized JSON files and for retrieving results after replay.

## Result retrieval

After `compiler().replay()`, computed polynomial values can be read back from `replay_outputs_integer.json`.

| Function | Signature                                     | Description                                                   |
| -------- | --------------------------------------------- | ------------------------------------------------------------- |
| `result` | `(const string& name, Polynomial& p) -> bool` | Retrieve a single polynomial result. Returns true on success. |
| `result` | `(const string& name, MRP& m) -> bool`        | Retrieve a multi-residue polynomial result.                   |
| `result` | `(const string& name, MRPArray& arr) -> bool` | Retrieve an MRP array result.                                 |

Only outputs with `"status": "computed"` in the replay output file are returned.

## Internal serialization functions

These are called automatically during `compiler().stop()` in FHETCH mode but can also be invoked explicitly.

| Function                   | Signature        | Description                                                                                                                    |
| -------------------------- | ---------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| `save_input_data`          | `() -> void`     | Write per-input JSON files and a master index to the program directory.                                                        |
| `save_probe_outputs`       | `() -> void`     | Write output probe definitions to `<prog>.outputs.json`.                                                                       |
| `reset_for_epoch`          | `() -> void`     | Clear input/output registries, reset the address allocator to 0, invalidate replay cache. Called automatically between epochs. |
| `get_input_ring_dimension` | `() -> uint64_t` | Ring dimension from the first registered input (0 if none). Used for the synthetic crypto context.                             |

## File I/O

### Binary format (not yet implemented)

| Function                                    | Signature                            | Status |
| ------------------------------------------- | ------------------------------------ | ------ |
| `save_polynomial` / `load_polynomial`       | `(Polynomial&, const path&) -> bool` | Stub   |
| `save_scalar` / `load_scalar`               | `(Scalar&, const path&) -> bool`     | Stub   |
| `save_mrp` / `load_mrp`                     | `(MRP&, const path&) -> bool`        | Stub   |
| `save_mrs` / `load_mrs`                     | `(MRS&, const path&) -> bool`        | Stub   |
| `save_srp_array` / `load_srp_array`         | `(SRPArray&, const path&) -> bool`   | Stub   |
| `save_mrp_array` / `load_mrp_array`         | `(MRPArray&, const path&) -> bool`   | Stub   |
| `save_mrp_dir` / `load_mrp_dir`             | `(MRP&, const path&) -> bool`        | Stub   |
| `save_mrp_array_dir` / `load_mrp_array_dir` | `(MRPArray&, const path&) -> bool`   | Stub   |

### JSON format (implemented)

| Function               | Signature                                         | Description                                                               |
| ---------------------- | ------------------------------------------------- | ------------------------------------------------------------------------- |
| `save_polynomial_json` | `(const Polynomial& p, const path& file) -> bool` | Human-readable polynomial export (ring_dim, format, number_type, values). |
| `load_polynomial_json` | `(Polynomial& p, const path& file) -> bool`       | Load from JSON.                                                           |
| `save_mrp_json`        | `(const MRP& m, const path& file) -> bool`        | All residues + base metadata.                                             |
| `load_mrp_json`        | `(MRP& m, const path& file) -> bool`              | Load from JSON.                                                           |
| `save_mrp_array_json`  | `(const MRPArray& arr, const path& file) -> bool` | Array of MRPs.                                                            |
| `load_mrp_array_json`  | `(MRPArray& arr, const path& file) -> bool`       | Load from JSON.                                                           |

## Hardware ISA mapping

The Generator (`src/Frontend/llvm/Generator.h`) emits instructions that map to the hardware ISA defined in the device spec (`devices/fhetch_sim/spec.yaml`). The full instruction set supported by FHETCH_SIM:

| ISA opcode      | Generator method                         | FHETCH baseline instruction                              |
| --------------- | ---------------------------------------- | -------------------------------------------------------- |
| ADD             | `add(dest, src1, src2, mod)`             | `sr_addp`                                                |
| SUB             | `sub(dest, src1, src2, mod)`             | `sr_subp`, `sr_negp` (as 0-a)                            |
| MUL             | `mul(dest, src1, src2, mod)`             | `sr_mulp`                                                |
| ADDI            | `addi(dest, src, imm, mod)`              | `sr_addps`, `sr_addps_coeff`                             |
| SUBI            | `subi(dest, src, imm, mod)`              | `sr_subps`, `sr_subps_coeff`                             |
| MULI            | `muli(dest, src, imm, mod)`              | `sr_mulps`                                               |
| NTT1 + NTT2     | `ntt(dest, src, mod, omega)`             | `sr_ntt`                                                 |
| INTT1 + INTT2   | `intt(dest, src, mod, omega)`            | `sr_intt`                                                |
| MORPH1 + MORPH2 | `morph(dest, src, mod, mask, logn, rot)` | `sr_permute`, `sr_automorph_*`, `sr_rot_automorph_coeff` |
| COPY            | (internal)                               | Register-to-register copy (optimization pass)            |
| MOVE            | (internal)                               | Memory move (optimization pass)                          |
| ALLOCATE        | (internal)                               | Memory allocation marker                                 |
| COMMENT         | (internal)                               | Annotation in instruction stream                         |
| IP              | (internal)                               | Instruction pointer                                      |
| START           | (internal)                               | Start marker                                             |
| STOP            | `halt()`                                 | Program termination                                      |
| FENCE           | (internal)                               | Memory fence                                             |
| NOP             | (internal)                               | No-operation                                             |
| HALT            | (internal)                               | Hardware halt signal                                     |

Instructions marked (internal) are emitted by the optimization pipeline or runtime infrastructure, not directly by FHETCH API calls.

## Implementation notes

- Non-integer variants emit the same opcodes as integer variants but with mod=0, signaling non-integer mode to the backend.
- `sr_negp` allocates a zero polynomial and emits `zero()` + `sub()`, rather than having a dedicated negate opcode.
- NTT omega values are computed via NTL's primitive root finder and cached per modulus in a static map.
- Synthetic addresses are allocated from a global atomic counter starting at 0, incrementing by 1 per polynomial/scalar. These stay below PHYS_REG_BASE (0x1000000000) so the optimizer's address tracking works correctly.
- FHETCH mode activates automatically on the first `fhetch::` function call. No explicit mode flag is needed.
