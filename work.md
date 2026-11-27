# WCCS Multi-Platform Library — Design Summary (English)

> Condensed English edition of the WCCS design manual (v1.0). The original
> Chinese long-form manual is kept as [`work.zh.md`](work.zh.md) and remains
> the authoritative reference; this document captures the decisions and
> contracts needed to implement and maintain the library.

**Target languages**: C / C++ / Rust / Python / C# / Node.js
**Core principle**: **one C core, six thin bindings**. Algorithm logic is
written once; every other language only translates ABI types, memory
ownership and errors.

---

## 0. Why a C core

The only stable multi-language contract is a **versioned C ABI**. It is the
common meeting point for Go, Java/JNI, C#, Dart, Swift, Kotlin, Zig and R, and
C++/Rust can consume it directly. Radix's design validates this path: core
separated from interface, UniFFI for Swift/Kotlin/C#/Python, C ABI + WASM for
TypeScript and the web.

| Layer | Responsibility | Forbidden |
|---|---|---|
| **Core (C)** | all WCCS state, Fenwick tree, cooldown queue, credit rebuild, random mapping | depending on any host-language runtime |
| **Bindings** | type mapping, ownership translation, error conversion, package metadata | duplicating algorithm logic |
| **Random source** | injected into the core; a binding may supply the implementation | bindings consuming randomness themselves |

Rust may *implement* the core but must export `extern "C"` rather than rely on
its unstable native ABI. If the core is plain C, the C++/Rust bindings are a
zero-cost header include or `bindgen` output. The default path is
**C core + Rust-assisted binding generation**; the core itself may be pure
C99.

---

## 1. Core specification (language-agnostic)

This section is the single behaviour every language must reproduce exactly.
Bindings must not deviate.

### 1.1 Inputs and state

```text
Inputs
  n           int32  > 0          number of elements
  w[n]        uint64 > 0          base weights
  L           int32  >= 0         cooldown window
  beta_q      uint64 Q32 [0,2^32] soft cooldown factor (0 = strict, 1 = off)
  alpha_q     uint64 Q32 [0,2^32] credit correction strength
  R           int32  >= 1         credit rebuild period
  seed        uint64              random seed
  stream_id   uint64              parallel stream id

State
  bit[]       uint64  Fenwick tree over current dynamic weights v_i
  total       uint64  Fenwick sum
  actual[n]   uint64  observed pick counts
  credit[n]   int64   Q32 credit
  cooldown[n] uint64  Q32 factor, 1<<32 means available
  recent[L]   int32   ring buffer of the last L picks
  step        uint64  total number of samples
  rng_state           injected random source
```

**Why integer/fixed point**: floating-point rounding, extended precision and
`-ffast-math` differ across compilers, which destroys reproducibility. All
weights and factors use `uint64` / `int64` fixed point and the Fenwick tree is
integer, so the same seed yields the identical index sequence on every
language and platform.

### 1.2 Dynamic weight

```text
v_i = max(0, w_i + ((alpha_q * credit_i) >> 64)) * cooldown_i >> 32
```

`credit_i` is Q32 (`(target_i - actual_i) << 32`) and `alpha_q` is Q32, so
their product is Q64 and must be reduced by `>> 64` to recover an integer
count adjustment. The Q32 cooldown factor is then applied with `>> 32`. Use
128-bit intermediates; never overflow.

> `credit_i` and `alpha_q` are both Q32, so their product is Q64 and the
> reduction is `>> 64`; the Q32 cooldown factor is applied separately with
> `>> 32`. A strictly positive soft factor must not truncate a positive base
> to zero. The binding ruling is [`docs/CONTRACT.md`](docs/CONTRACT.md) §3.

### 1.3 Unbiased bounded mapping (Lemire)

The random source only provides `next_u64()`. The core maps it without
modulo bias:

```c
static uint64_t random_below(uint64_t n, wccs_rng_t *rng) {
    uint64_t x = rng->next_u64(rng->ctx);
    __uint128_t m = (__uint128_t)x * n;
    uint64_t l = (uint64_t)m;
    if (l < n) {
        uint64_t t = (0 - n) % n;
        while (l < t) {
            x = rng->next_u64(rng->ctx);
            m = (__uint128_t)x * n;
            l = (uint64_t)m;
        }
    }
    return (uint64_t)(m >> 64);
}
```

**Never use `% n` on the random value.** No binding may generate indices
itself; all sampling goes through `wccs_sample`.

### 1.4 Sampling

```text
if total == 0:
    release the window (cooldown = 1<<32, cool_count = 0), rebuild Fenwick
    exclude the previous index from this draw   # CONTRACT §4

r = random_below(total)
i = fenwick_find_by_cumulative(r)

actual[i] += 1
step += 1

if recent_count == L:                # release oldest before writing (CONTRACT §2)
    j = recent_pop_oldest()
    if --cool_count[j] == 0:
        cooldown[j] = 1 << 32
        fenwick_update(j, calc_v(j))
cooldown[i] = beta_q
cool_count[i] += 1
fenwick_update(i, calc_v(i))
recent_push(i)

if step % R == 0: rebuild_credit()
return i
```

> The window is released *before* the new index is written, and it counts
> occurrences: an index stays cooled until its last occurrence leaves the
> window. This is what makes `beta_q > 0` well defined when the same index is
> drawn twice inside one window. See [`docs/CONTRACT.md`](docs/CONTRACT.md) §2.

### 1.5 Credit rebuild

```text
target_i = (w_i * step) / W_total        # integer division, W_total = sum(w)
credit_i = (int64)(target_i << 32) - (actual_i << 32)
v_i      = calc_v(i)
fenwick_rebuild(v)
```

`credit_i` is "target count minus actual count" expressed in Q32.
`w_i * step` needs 128-bit intermediates to avoid overflow.

### 1.6 Degeneracy table (must match in every language)

| Configuration | Behaviour |
|---|---|
| `L=0, alpha_q=0` | pure weighted random, repeats allowed |
| `L=1, beta_q=0` | no consecutive repeats |
| `L>=n, beta_q=0`, equal weights | a full random permutation every round |
| `beta_q>0` | soft cooldown; high weights are not starved |
| `total=0` | release the window, rebuild Fenwick, exclude the previous index from the new round (see [`docs/CONTRACT.md`](docs/CONTRACT.md) §4) |

> With `L >= n`, "the last L draws are distinct" is unsatisfiable for a
> population of `n`. The contract degrades to: every block of `n` draws is a
> random permutation, and consecutive repeats never occur for any `L`.
> Repeats at distances `2..L` are a mathematical consequence, not a violation.

---

## 2. C ABI

`include/wccs.h` is the **only stable contract** (ABI version 1). It can be
generated by `cbindgen` from annotated Rust or maintained by hand.

```c
#define WCCS_ABI_VERSION 1
typedef struct wccs_ctx wccs_ctx;              /* opaque handle */

typedef struct {
    void *ctx;
    uint64_t (*next_u64)(void *ctx);
} wccs_rng_t;

WCCS_API wccs_ctx* wccs_new(int32_t n, const uint64_t *w,
    int32_t L, uint64_t beta_q, uint64_t alpha_q,
    int32_t R, uint64_t seed, uint64_t stream_id,
    wccs_rng_t rng, int32_t *err);

WCCS_API int32_t wccs_sample(wccs_ctx *c, int32_t *err);
WCCS_API void    wccs_free(wccs_ctx *c);
WCCS_API int32_t wccs_abi_version(void);
```

**ABI rules**

- Append only: never change an existing signature or enum value.
- `err` uses stable codes (`WCCS_OK=0`, `WCCS_E_INVAL=1`, ...); new codes are
  appended without renumbering.
- Opaque handle + constructor/destructor; internal layout is never exposed.
- **Core-allocated memory is freed by the core.** Bindings never call the host
  allocator on a core pointer. Any future function returning a string/array
  must ship with a matching free function.
- **No panic or exception may cross the C ABI.** A Rust core wraps every
  export in `catch_unwind` and converts to an error code; a C core reports via
  the `err` out-parameter.

---

## 3. Binding notes

### 3.1 C / C++

- **C**: include `wccs.h`, link the static or shared library. Zero-cost.
- **C++**: wrap the C ABI in a thin RAII class. Do **not** introduce virtual
  functions, exceptions or templates *at the boundary* — the boundary only
  translates. Host-side exceptions (`std::runtime_error`) are fine.
- Build with CMake or Meson; ship `pkg-config` and CMake config files.
  Windows: `.lib` import library + `.dll` with `__declspec(dllexport)`.

### 3.2 Rust

- **As core implementer**: `crate-type = ["cdylib", "staticlib"]`, export
  `extern "C"`, generate the header with `cbindgen`.
- **As consumer**: generate FFI with `bindgen` from `wccs.h`, then add a safe
  wrapper. Avoid moving a C core into Rust and back — every extra ABI hop is
  another failure mode.
- Publish `.a`/`.so`/`.dylib`/`.dll` as release artifacts; the crate itself
  is an `rlib`.

### 3.3 Python

- Prefer **ctypes/cffi over PyO3** when the core is already a C ABI: the
  binding stays decoupled and Python upgrades need no recompilation.
- If PyO3 is required, package with **maturin** (PEP 517, CPython 3.8+,
  manylinux for Linux wheels).
- **The binding performs no numeric work**: construct arguments, hold the
  handle, call `wccs_sample`, translate errors to exceptions.

### 3.4 C#

- Prefer **P/Invoke** on the C ABI; it is the most stable path and the most
  Native-AOT friendly.
- Use `[LibraryImport]` (.NET 8+ source generation) instead of `[DllImport]`
  to bind at compile time and avoid trimmer surprises.
- NuGet: place native libraries under `runtimes/{rid}/native/`; never
  statically link them into the managed assembly, which breaks RID
  resolution.

### 3.5 Node.js

- **Node/Bun only**: build a `.node` addon with **napi-rs** or
  **node-addon-api**. Publish per-platform optional packages
  (`@scope/wccs-linux-x64-gnu`, ...); the root package ships only a loader.
- **Deno / Edge Workers / browsers**: prefer **WASM** via `wasm-bindgen`.
- Packaging: keep `.node` files external to bundles, provide both CJS and ESM
  loaders with an explicit `exports` map, and guard WASM with a feature flag.

---

## 4. Random source and reproducibility

### 4.1 Injection

The core embeds no randomness; it holds only a `wccs_rng_t` function pointer.
Each binding may provide the host RNG:

| Language | Default injection |
|---|---|
| C/C++ | PCG64 or Philox (reference implementation shipped with the core) |
| Rust | `rand::rngs::SmallRng` / `rand_pcg` |
| Python | `random.Random.getrandbits(64)` |
| C# | `System.Random.NextInt64` / `RandomNumberGenerator` |
| Node.js | `crypto.randomBytes` / `seedrandom` |

**Key constraint**: to reproduce the same sequence across languages, every
binding must inject the *same* deterministic PRNG (for example the core's
PCG64 reference), not the host default.

### 4.2 Reproducibility levels

| Level | Guarantee | Method |
|---|---|---|
| L0 | same language, same platform | fixed seed + host RNG |
| L1 | same language, cross-platform | integer/Q32 + unbiased mapping |
| **L2** | **cross-language, cross-platform** | L1 + one shared deterministic PRNG + zero floating point |

WCCS is designed for **L2**. A binding that cannot reach L2 must document the
downgrade prominently in its README.

---

## 5. Test matrix (all green before release)

### 5.1 Core logic (C, authoritative)

| Test | Method | Pass criterion |
|---|---|---|
| Frequency | 10⁶ draws, chi-square | deviation from `w_i/Σw` < ε |
| Cooldown | `L=1, beta_q=0` | zero consecutive repeats |
| Coverage | `L>=n`, equal weights | each `n` draws cover the whole set |
| Boundaries | `n=1`, `L=0`, `L>n`, `total=0` | no crash, matches degeneracy table |
| Unbiased mapping | `n=3,5,7`, 10⁶ draws each | uniform index frequency |
| Reproducibility | two contexts, same seed | bit-identical sequences |

### 5.2 Cross-language consistency

For each binding, run the **same seed and parameters** for N draws, hash the
index sequence with SHA-256 and compare against the C reference.

```text
Required vector:
  seed=42, stream_id=0, n=10, w=all 1, L=3, beta_q=0, alpha_q=0, R=1000
  SHA-256 of the first 100 indices: <expected>
```

A language that cannot reach L2 is downgraded to "same-language reproducible"
and must say so in its README.

### 5.3 Boundary tests at the binding layer

- NULL pointers, invalid `n`, `w_i=0`, `beta_q > 2^32` → error code, no crash.
- Errors surface as Python exceptions, `WccsException` in C#, `Error` in Node,
  `std::runtime_error` in C++.
- Concurrent use of one handle must be documented as unsafe or guarded by a
  lock.

### 5.4 Packaging acceptance

| Language | Acceptance |
|---|---|
| C/C++ | `pkg-config --cflags --libs wccs` and CMake `find_package` work |
| Rust | `cargo test`; release build emits `.a`/`.so`/`.dylib`/`.dll` |
| Python | `pip install wccs` then `import wccs`; wheel passes manylinux audit |
| C# | NuGet package passes `dotnet test` on `win-x64`, `linux-x64`, `osx-arm64`; no Native-AOT trim warnings |
| Node.js | Node LTS / Bun / Deno each run once; CJS and ESM `import`/`require` tested |

---

## 6. CI pipeline

### 6.1 Build matrix

```text
Platforms: ubuntu-22.04, macos-15, windows-2022
Arches:    x86_64, aarch64 (macOS/Linux)
Languages: C (core), C++, Rust, Python 3.9–3.13, .NET 8/10, Node 20/22
```

> `macos-14` was retired by GitHub on 2026-11-02 and can no longer be used.

### 6.2 Stages

1. **Core build and test** (triggers downstream)

   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ctest --test-dir build --output-on-failure
   ```

2. **Binding builds** (parallel): `maturin build --release` (manylinux on
   Linux), `dotnet pack` with native libs under `runtimes/`,
   `napi build --platform --release`, `cargo build --release` + `cbindgen`.

3. **Cross-language consistency**: run every binding on the fixed vector and
   compare SHA-256 with the C reference.

4. **Publish**: PyPI (`maturin publish` / `uv publish`), NuGet
   (`dotnet nuget push`), npm (root + platform packages), crates.io
   (`cargo publish`), GitHub Release for C/C++ binaries.

**Version lock**: all binding packages share the core ABI major version. When
`wccs_abi_version()` returns `1`, every language package must resolve `wccs`
to a range that targets ABI 1.

---

## 7. Review checklist

- [ ] ABI header has a version macro; enums are append-only.
- [ ] Every export has an error boundary (`catch_unwind` in Rust); no panic
      crosses the ABI.
- [ ] Core memory is released only by `wccs_free`; bindings contain no `free`.
- [ ] Every binding's RNG satisfies the declared reproducibility level.
- [ ] `random_below` lives in the core; no binding generates indices.
- [ ] Weights, factors and Fenwick tree are integer/fixed point.
- [ ] Python wheel passes the manylinux audit.
- [ ] C# Native AOT produces no trim/AOT warnings.
- [ ] Node native addon stays external to bundles; WASM fallback loads.
- [ ] Cross-language fixed-vector SHA-256 matches (or the downgrade is
      documented).
- [ ] Cooldown, coverage, boundary and frequency tests pass.
- [ ] Error codes and exception mappings agree across languages.

---

**One-line summary**: 70% of the effort is the determinism of the C core and
the WCCS logic; 30% is binding and packaging. Keep bindings as thin as
possible — copying algorithm logic into a binding is the root cause of
cross-language divergence and long-term maintenance cost.
