# Implementation notes — deviations from `work.md`

`work.md` is the design manual. This repository is a concrete, tested
implementation of it. A few places required an engineering decision because
the manual's pseudocode was ambiguous or contradictory with its own
degeneracy table. Every decision below keeps the **single C core** as the
only source of truth, so all bindings remain byte-identical regardless.

## 1. Cooldown window ordering (`work.md` §1.4 vs §1.6)

The manual writes:

```text
recent_push(i);
if (recent_count == L) { j = recent_pop_oldest(); restore(j); }
```

Taken literally with `L=1, beta_q=0`, the just-sampled index is immediately
restored, so consecutive repeats would be allowed — contradicting §1.6
("`L=1, beta_q=0` → 禁止连续相同") and §5.1.

**Implemented**: the window has capacity `L`; when it is full, the oldest
entry is released *before* the new index is written. Equivalently:
"push, then release while `count > L`". Both forms keep exactly the most
recent `L` indices cooled. This satisfies the full degeneracy table:

| Config | Observed |
|---|---|
| `L=0, alpha_q=0` | pure weighted random, repeats allowed |
| `L=1, beta_q=0` | zero consecutive repeats over 20 000 draws |
| `L>=n, beta_q=0`, equal weights | each round of `n` is a permutation |
| `total=0` | cooldown queue cleared, Fenwick rebuilt |

`L=0` bypasses the cooldown path entirely (including `beta_q`), which is
required for the "pure weighted random" row.

## 2. Fixed-point reduction in `calc_v` (`work.md` §1.2)

The manual writes `v_i = max(0, w_i + (alpha_q * credit_i) >> 32) * cooldown_i >> 32`.
But §1.1/§1.5 define `credit_i` as **Q32** (`(target_i - actual_i) << 32`)
and `alpha_q` as **Q32** in `[0, 2^32]`. Their product is Q64; reducing by
only 32 leaves a Q32 quantity that cannot be added to an integer weight.

**Implemented** (dimensionally consistent, all integer):

```text
delta = (alpha_q * credit_i) >> 64        # Q32 * Q32 -> Q64 -> integer count
base  = max(0, w_i + delta)
v_i   = (base * cooldown_i) >> 32         # Q32 cooldown factor
```

All products use the portable 128-bit helpers in `src/wccs.c`. Because every
binding calls the same C function, this choice is part of the ABI's
behavioural contract, not a per-language decision.

## 3. Portable 128-bit arithmetic instead of `__int128`

§1.2/§1.3 suggest `__int128`. MSVC (used by the Rust `-msvc` toolchain and
Windows CI) does not provide it. The core therefore implements `mul128`,
`mul128_shr32` and `div128_64` in pure C99 with 32-bit limbs. This also makes
the core trivially compilable to `wasm32`. The Lemire unbiased mapping uses
`{hi, lo} = x * n` and returns `hi`; no `% n` on the random value.

## 4. ABI additions (append-only, §2.1)

`wccs_new` with an injected `wccs_rng_t` is implemented exactly as specified.
Four additions were necessary and follow the "append only" rule:

| Addition | Reason |
|---|---|
| `wccs_new_default` | Every binding must share **one** deterministic engine for L2; a convenience constructor is safer than re-implementing PCG in six languages. |
| `wccs_pcg64_init` / `wccs_pcg64_next` | The manual requires a core-supplied reference PRNG (§4.1). |
| `wccs_error_string`, `wccs_step`, `wccs_total`, `wccs_last_error` | Error-message translation and test/debug helpers. `wccs_error_string` returns a static string (never freed). |

No function currently returns a heap-allocated string or array, so
`wccs_free_string` / `wccs_free_array` are not needed yet. If such a function
is appended, its matching free function must be appended in the same ABI
version (§2.2).

## 5. Reference PRNG choice (§4.1)

The manual suggests "PCG64 或 Philox". This implementation uses **PCG
RXS-M-XS 64/64** (64-bit state, 64-bit output, `PCG_MULT`/`PCG_M2`): it is
fully deterministic, needs only 64-bit multiplies (no 128-bit state), and is
portable to MSVC and WASM. `seed` and `stream_id` are mixed into the initial
state with an 8-step warm-up.

## 6. Node.js runtime path (§3.5)

The manual prefers WASM for Deno/Edge/browser coverage. This repository
provides both:

- **Node / Bun**: `index.cjs` loads the native C library through `koffi`
  (zero compilation, CJS + ESM loaders, `exports` map). This is the path
  verified locally and in CI.
- **Deno / Edge Workers / browsers**: `wasm/build.sh` compiles the unchanged
  C core with Emscripten (`WASM_BIGINT=1`), and `wasm/loader.mjs` exposes the
  same `Sampler` API. The generated `.wasm`/`.mjs` are CI artifacts and are
  intentionally not committed.

A native `napi-rs` addon could replace koffi as the high-performance Node
path without touching the core.

## 7. Testing framework choices (§5)

- C core: self-contained assertions + `ctest` (48 793 checks).
- Rust: `cargo test` (4 tests).
- Python: stdlib `unittest` (6 tests).
- C#: a self-contained console runner (`dotnet run`) instead of MSTest/xUnit,
  so `dotnet test`-style verification needs no NuGet test packages.
- Node: `node:assert` via `npm test`, both CJS and ESM.

All seven entry points emit the same 100-index vector and are compared by
`tests/run_cross_lang.py` against the pinned SHA-256 in
`tests/vectors/v1.txt`.

## 8. Known constraints

- A single `wccs_ctx` is **not thread-safe** (documented, not locked).
- `Σ w_i` must fit in `uint64` (`WCCS_E_OVERFLOW` otherwise); `Σ v_i` must
  also fit in `uint64`.
- `beta_q` / `alpha_q` are validated against `2^32` inclusive.
- Using `wccs_new` with a custom RNG downgrades reproducibility to L0/L1
  unless the same engine is injected in every language (§4.2).
