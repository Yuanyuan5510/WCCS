# WCCS C ABI contract

`include/wccs.h` is the **only stable API surface**. Version: `WCCS_ABI_VERSION 1`.

This file defines *what symbols exist*. [`CONTRACT.md`](CONTRACT.md) defines
*what they must do* where `work.md` is silent or self-contradictory; it is
authoritative over the manual and governs every binding test vector.

## Rules

1. **Append only.** Never change an existing signature, struct layout or enum
   value. New functions/error codes are appended.
2. **Opaque handle.** `wccs_ctx` is never defined in the header. Constructors
   return it; `wccs_free` releases it.
3. **Memory ownership.** Memory allocated by the core is released by the
   core. Bindings must never call `free`/`delete` on a core pointer.
   `wccs_error_string` returns a static string (do not free).
4. **No exceptions/panics across the ABI.** Errors are returned through the
   `int32_t *err` out parameter; `wccs_sample` returns `-1` on error.
5. **Integer / Q32 only.** No floating point anywhere in the core.

## Functions

| Function | Purpose |
|---|---|
| `wccs_new` | Construct with an injected `wccs_rng_t` (advanced / L0–L1). |
| `wccs_new_default` | Construct with the core reference PCG64 (**L2**, recommended). |
| `wccs_sample` | Draw one index; returns `-1` and sets `err` on failure. |
| `wccs_free` | Release the context (no-op on `NULL`). |
| `wccs_abi_version` | Returns `1`. |
| `wccs_error_string` | Static human-readable message for an error code. |
| `wccs_step` / `wccs_total` / `wccs_last_error` | Read-only state helpers. |
| `wccs_pcg64_init` / `wccs_pcg64_next` | Reference PRNG, useful when building a custom `wccs_rng_t`. |
| `wccs_stream_check` | Enable the optional duplicate `(seed, stream_id)` detector; returns the previous mode (`0` off, `1` warn, `2` error). See `CONTRACT.md` §5. |
| `wccs_stream_check_mode` | Read the current detector mode. |

## Error codes (stable, append only)

| Code | Value | Meaning |
|---|---|---|
| `WCCS_OK` | 0 | success |
| `WCCS_E_INVAL` | 1 | `n<=0`, `w==NULL`, any `w_i==0`, `L<0`, `R<1`, `beta_q`/`alpha_q > 2^32`, missing RNG callback |
| `WCCS_E_NOMEM` | 2 | allocation failure, or `n * sizeof(uint64_t)` exceeds the target's `size_t` (32-bit builds) |
| `WCCS_E_OVERFLOW` | 3 | `Σ w_i` exceeds `uint64` |
| `WCCS_E_EMPTY` | 4 | all dynamic weights are zero (after cooldown reset) |
| `WCCS_E_NULL` | 5 | `NULL` context passed to a method |
| `WCCS_E_STATE` | 6 | internal invariant violated (defensive) |
| `WCCS_E_STREAM_CONFLICT` | 7 | two live contexts share `(seed, stream_id)`; only when `wccs_stream_check(2)` is active |

Binding-layer exception mapping (§5.3):

| Language | Mapping |
|---|---|
| C | `err` out parameter |
| C++ | `wccs::Error` (`std::runtime_error`) |
| Rust | `wccs::Error` (`Result`) |
| Python | `wccs.WccsError` |
| C# | `Wccs.WccsException` |
| Node.js | `WccsError` |

## Parameters

```text
n            int32  > 0        number of elements
w[n]         uint64 > 0        base weights (copied into the context)
L            int32  >= 0       cooldown window length
beta_q       uint64 [0, 2^32]  soft cooldown factor, Q32 (0 = strict, 2^32 = off)
alpha_q      uint64 [0, 2^32]  credit correction strength, Q32
R            int32  >= 1       credit rebuild period (in samples)
seed         uint64            reference PRNG seed
stream_id    uint64            parallel stream id / PRNG stream selector
```

## Thread safety

One `wccs_ctx` is **not thread-safe**. Use one context per thread and give
each thread a **distinct `stream_id`**; add external locking if a context must
be shared.

Two live contexts with the same `(seed, stream_id)` produce identical
sequences. That is required for L2 reproducibility tests and is therefore
allowed by default, but it is a silent error in a parallel run. Enable the
optional detector before creating contexts:

```c
wccs_stream_check(2);   /* 2 = fail with WCCS_E_STREAM_CONFLICT, 1 = warn, 0 = off */
```

Serial reuse of a key (free, then construct again) is always legal. See
`CONTRACT.md` §5 for the registry size, thread-safety and fallback rules.

## Windows export macro

- Building the DLL: define `WCCS_BUILD_DLL` → `__declspec(dllexport)`.
- Consuming the DLL: no definition → `__declspec(dllimport)`.
- Static linking: define `WCCS_STATIC` → plain declarations.
  `wccs_static` exported by CMake propagates this definition automatically.

## Version lock

The root [`VERSION`](../VERSION) file is the single source of truth. All
binding packages (`pyproject.toml`, `Cargo.toml`, `package.json`) are at
`1.0.2` and target ABI 1; `CMakeLists.txt` reads `VERSION` directly.

CI enforces the rule before building anything:

- `VERSION` must equal the version in every package manifest;
- on a tag, `v<tag>` must equal `VERSION`.

A mismatch fails the `version gate` job, so no artifact and no release is
produced. Any ABI bump requires a coordinated major-version bump here and in
all six packages (§6).
