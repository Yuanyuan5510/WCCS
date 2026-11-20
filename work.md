# *WCCS Multi-Platform Library Authoring Manual*

**Applicable Languages**: C / C++ / Rust / Python / C# / Node.js

**Core Principles**: **One C core, six thin binding layers**. Algorithm logic is written only once; other languages ​​only perform ABI translation and package encapsulation.

## 0. Architectural Decision: Why a C Core is Essential

The most stable contract for a multi-platform library is not "writing the algorithm once for each language," but rather **a versioned C ABI**. The C ABI is a common intersection point for languages ​​such as Go, Java/JNI, C#, Dart, Swift, Kotlin, Zig, and R, and is also a stable layer that C++/Rust can directly consume. Radix's practices validate this path: core and interface separation; UniFFI serves Swift/Kotlin/C#/Python; C-ABI + WASM serves TypeScript and Web.

**Hard Constraints**:

| Layer | Responsibility | Prohibited |

|---|---|---|

| **Core (C)** | WCCS All State, Fenwick, Cooling Queue, Credit Reconstruction, Random Mapping | Prohibited from depending on any host language runtime |

| **Binding Layer** | Type Mapping, Memory Ownership Translation, Error Translation, Package Metadata | Prohibited from copying algorithm logic |

| **Random Source** | Called by the core via an injected interface; replaceable implementation in the binding layer | The binding layer is prohibited from consuming random numbers itself |

> Rust can be used as the **implementation language** for the core, but it must export the `extern "C"` C ABI externally, rather than relying on Rust's unstable native ABI. If the core is written directly in C, C++/Rust bindings are generated at zero cost via header file inclusion or `bindgen`. **This manual uses "C core + Rust auxiliary binding generation" as the default path**; the C core can also be implemented using pure C99.

## 1. WCCS Core Logic Specification (Language Independent)

This section is the **only specification that all languages ​​must implement verbatim**. The binding layer must not deviate from it.

### 1.1 Input and State

```text
Input:

n: Set size (int32, > 0)

w[n]: Base weight, integer (uint64), > 0

L: Cooling window (int32, >= 0)

beta_q: Soft cooling factor, fixed-point Q32 (uint64), range [0, 2^32]

beta_q = round(beta * 2^32), beta=0 strict cooling, beta=1 no cooling

alpha_q: Credit correction strength, fixed-point Q32 (uint64), range [0, 2^32]

R: Credit reconstruction period (int32, >= 1)

seed: Random seed (uint64)

stream_id: Parallel stream number (uint64)

State:

Fenwick bit[]: Length n+1, Fenwick Tree, storing the current dynamic weight v_i

total: Fenwick total (uint64)

actual[n]: Actual number of samplings (uint64)

credit[n]: Credit value, fixed-point Q32 (int64)

cooldown[n]: Cooling factor, fixed-point Q32 (uint64), 1<<32 indicates availability

recent[L]: Circular buffer, storing the index of the most recent L samplings (int32)

recent_head: Circular buffer write position

recent_count: Number of elements in the current window

step: Total number of samplings (uint64), used for credit reconstruction

rng_state: Injected random source state (managed by the binding layer)

**Why use integers/fixed-point**: Cross-language floating-point operations (rounding of `double`, extended precision, `-ffast-math`) can lead to unreproducible results. Weights and factors are all fixed-point (`uint64` / `int64`), while Fenwick uses integers, ensuring that **the same seed produces the exact same index sequence across all languages ​​and platforms**. Randompack's cross-platform reproducibility is built upon separating the engine and distribution, and strictly controlling the numerical path.

### 1.2 Dynamic Weight Calculation

```text
v_i = max(0, w_i + (alpha_q * credit_i) >> 32) * cooldown_i >> 32

```

All multiplications use `__int128` or high-precision intermediate types to prevent overflow; `>> 32` is a Q32 fixed-point restoration.

### 1.3 Unbiased Random Mapping (Lemire)

The random source only provides `next_u64()`. The core must use unbiased mapping:

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

**Prohibit `% n`**. The binding layers for all languages ​​must not generate random indices themselves; they must use the core `wccs_sample`.

### 1.4 Sampling

```c

// Pre-processing: If total == 0, clear cooldown and rebuild

uint64_t r = random_below(total, rng); // Unbiased in [0, total)

int32_t i = fenwick_find_by_cumulative(r); // Fenwick prefix sum location

actual[i] += 1;

step += 1;

// Cooldown update

cooldown[i] = beta_q; // beta_q=0 strict, beta_q=1<<32 no cooldown

fenwick_update(i, calc_v(i));

recent_push(i); // Circular buffer

if (recent_count == L) {

int32_t j = recent_pop_oldest();

cooldown[j] = 1ULL << 32;

fenwick_update(j, calc_v(j));

}
// Credit Reconstruction

if (step % R == 0) rebuild_credit();

return i;

```

### 1.5 Credit Reconstruction

```text

target_i = (w_i * step) / W_total // Integer division, W_total = Σw

credit_i = (int64)(target_i << 32) - (actual_i << 32)

v_i = calc_v(i)

fenwick_rebuild(v)

```

`credit_i` is "target number of times - actual number of times", using Q32 to represent the decimal part. `target_i` is calculated using `uint64` to prevent overflow.

### 1.6 Degenerate Behavior (Must be consistent across languages)

| Configuration | Behavior |

|---|---|

| `L=0, alpha_q=0` | Purely weighted random, allows repetition |

| `L=1, beta_q=0` | Prohibits consecutive identical occurrences |

| `L≥n, beta_q=0, w` Equal weights | Random permutation without replacement in each round |

| `beta_q>0` | Soft cooling, high weights will not be overwhelmed for long periods |

| `total=0` | Clear the cooldown queue, restore all `cooldown=1<<32`, rebuild Fenwick |

## 2. C Core Design (Sole Source of Facts)

### 2.1 Common Header Files (Versioned ABI)

`include/wccs.h` is the **single stable contract**. Generated from annotated Rust code using `cbindgen`, or maintained manually.

```c
#define WCCS_ABI_VERSION 1

typedef struct wccs_ctx wccs_ctx; // opaque handle

typedef struct { 
void *ctx; 
uint64_t (*next_u64)(void *ctx);
} wccs_rng_t;

WCCS_API wccs_ctx* wccs_new( 
int32_t n, const uint64_t *w, 
int32_t L, uint64_t beta_q, uint64_t alpha_q, 
int32_t R, uint64_t seed, uint64_t stream_id, 
wccs_rng_t rng, int32_t *err);

WCCS_API int32_t wccs_sample(wccs_ctx *c, int32_t *err);

WCCS_API void wccs_free(wccs_ctx *c);

WCCS_API int32_t wccs_abi_version(void);

```

**ABI Rules**:

- Only append, do not modify existing function signatures and enumeration values;

- `err` uses stable error codes (`WCCS_OK=0`, `WCCS_E_INVAL=1`, ...), appending a new code does not change the value of the old code;

- Opaque handle + constructor/destructor + method, do not expose the internal structure layout.

### 2.2 Memory Ownership

**Memory allocated by the kernel must be freed by the kernel.** The binding layer must not call the host language's `free` on pointers returned by the kernel.

```c

// The core internally uses malloc/free, and the binding layer releases data via wccs_free

// If the core returns a string/array, a matching wccs_free_string / wccs_free_array must be provided

```

### 2.3 Error Handling

**Panic/exception crossing the C ABI is absolutely prohibited.** The Rust core wraps each exported function with `catch_unwind` to convert it to an error code. The C core returns parameters via `err`.

## 3. Binding Layer Design for Each Language

### 3.1 C / C++

**C**: Directly `#include "wccs.h"` to link static or dynamic libraries. This is zero-cost binding.

**C++**: Wrap a RAII layer on top of the C ABI. **Do not introduce virtual functions, exceptions, or templates to the boundary layer**—the boundary layer only performs translation.

``` ```cpp
class Wccs { 
wccs_ctx* c_ = nullptr;
public: 
Wccs(int32_t n, const std::vector<uint64_t>& w, /* ... */) { 
int32_t err = 0; 
c_ = wccs_new(n, w.data(), L, beta_q, alpha_q, R, seed, stream_id, rng_, &err); 
if (err) th
row std::runtime_error("wccs init failed");

}
int32_t sample() { int32_t e=0; auto r=wccs_sample(c_,&e); if(e) throw ...; return r; }

~Wccs() { if (c_) wccs_free(c_); }

};

**Build and Release**:

- Use CMake or Meson to produce `libwccs.a` / `libwccs.so|dylib|dll` + header files;

- Provide a `pkg-config` file or CMake config;

- Windows uses `.lib` (import library) + `.dll`, and exports symbols using `__declspec(dllexport)`.

### 3.2 Rust

Rust can have two roles:

**Role A: Core Implementer** (Recommended for new projects) In `Cargo.toml`, `crate-type = ["cdylib", "staticlib"]` ​​exports `extern "C"` functions, which are then used by `cbindgen` to generate `wccs.h`.

``toml

[lib]

crate-type = ["cdylib", "staticlib"]

[features]

default = []

wasm = ["wasm-bindgen", "getrandom/wasm_js"]

```

**Role B: Consumer**. The Rust side uses `bindgen` to generate FFI bindings from `wccs.h`, then wraps them in a safe API. For long-term, Rust-focused projects, `cxx` can be used to bridge C++, but **it is not recommended** to move the core from C to Rust and back just to "use Rust"—each additional hop in the ABI layer adds another failure mode.

```toml

```crate-type = ["cdylib", "staticlib"]`

```crate-type = ["cdylib", "staticlib"]`

```cbindgen`

```toml

```cbindgen = ["cdylib", "staticlib"]`

```export ```extern "C"

`export `C"

```export `C` ... **Release**: `cargo build --release` produces `.a` / `.so` / `.dylib` / `.dll`. If also released to crates.io, the crate itself is `rlib`, and the native library is uploaded to the GitHub Release via CI as a release artifact. Downstream users can download it from the link.

### 3.3 Python

**Solution Selection**: If the core is a C ABI, the Python side should prioritize using **cffi** or **ctypes** to directly call the dynamic library, bypassing PyO3. This decouples Python bindings from the core, eliminating the need for recompilation during Python version upgrades.

If you insist on using PyO3 (e.g., wanting a direct Rust kernel on the Python side), package it using **maturin**:

```toml

# pyproject.toml

[build-system]

requires = ["maturin>=1.0,<2.0"]

build-backend = "maturin"

[tool.maturin]

features = ["pyo3"]

```

```bash

maturin build --release # Produce wheel

maturin publish # Publish to PyPI (Linux requires manylinux container)

```

maturin implements PEP 517, supports CPython 3.8+, and can be directly uploaded to PyPI; Linux wheels must be built using manylinux or `cargo-zigbuild`.

**Key point:** The Python binding layer **should not perform numerical operations**; all WCCS logic is in the kernel. Python is only responsible for constructing parameters, holding handles, calling `wccs_sample`, and converting errors to exceptions.

### 3.4 C#

**Please choose P/Invoke to directly call the C ABI.** This is the most stable and most friendly path to native AOT.

```csharp

[DllImport("wccs", CallingConvention = CallingConvention.Cdecl)]

static extern IntPtr wccs_new(int n, ulong[] w, int L, ulong beta_q, ulong alpha_q,

int R, ulong seed, ulong stream_id, ref WccsRng rng, out int err);

[DllImport("wccs", CallingConvention = CallingConvention.Cdecl)]

static extern int wccs_sample(IntPtr ctx, out int err);

```

**Native AOT Note**: P/Invoke is indistinguishable from non-AOT under Native AOT, but **unanalyzed P/Invoke may cause problems during pruning**. Solutions:

- Replace `[DllImport]` with `[LibraryImport]` (generated from .NET 8+ sources), binding at compile time;

- Or retain `[DllImport]` and explicitly configure `DynamicallyAccessedMembers` / `trimmer`.

**NuGet Packaging**: Native libraries are placed under `runtimes/{rid}/native/` by RID, automatically selected by NuGet. The `.targets` file is optional and used to handle transitive native dependencies. **Do not** statically link `NativeLibrary Include` into managed assemblies, as this will break cross-platform NuGet RID resolution.

### 3.5 Node.js

Node.js strategies depend on runtime scope:

**Serving only Node/Bun**: Build native `.node` plugins using **napi-rs** (Rust) or **node-addon-api** (C++). napi-rs automatically generates platform loaders, attempting local files → optional platform packages → WASI fallback. During deployment, optional packages (e.g., `@scope/wccs-linux-x64-gnu`) are split by platform, with the root package containing only the loader and metadata.

**Requires coverage of Deno / Edge Workers / browsers**: **WASM is prioritized**. The C kernel is compiled into `.wasm` files using `wasm-bindgen`. In Node scenarios, native plugins can be retained as a high-performance path; non-Node runtimes fall back to WASM.

**Packaging strategy**:

- The root package `main` points to the loader generated by napi-rs; `.node` files should not be included in the bundle, remaining external.

- Both CommonJS and ESM loaders are generated simultaneously; the `exports` field explicitly declares `import` / `require`.

- WASM builds use `--features wasm` conditional compilation to exclude file system dependencies.

## 4. Random Sources and Cross-Language Reproducibility

### 4.1 Random Source Injection

The core **does not have a built-in** random source; it only holds a function pointer to `wccs_rng_t`. Each binding layer is responsible for providing the host language's RNG:

| Language | Default Injection |

|---|---|

| C/C++ | PCG64 or Philox (reference implementation included with the core) |

| Rust | `rand::rngs::SmallRng` or `rand_pcg` |

| Python | `getrandbits(64)` of `random.Random` |

| C# | `NextInt64` of `System.Random` or `RandomNumberGenerator` |

| Node.js | `crypto.randomBytes` or `seedrandom` |

**Key Constraint**: If the goal is to **produce the same sequence across languages ​​with the same seed**, all binding layers must inject the **same deterministic PRNG** (e.g., the PCG64 reference implementation included with the core), not the host language's own RNG. Randompack's reproducibility is achieved through a unified engine, rather than relying on the platform's default random source. ### 4.2 Reproducibility Levels

| Level | Guarantee | Practice |

|---|---|---|

| L0 | Reproducible within the same language and platform | Fixed seed + host RNG |

| L1 | Reproducible across different platforms and within the same language | Integer/fixed-point arithmetic + unbiased mapping |

| L2 | **Reproducible across different languages ​​and platforms** | L1 + deterministic PRNG for unified injection + prohibition of any floating-point differences |

The WCCS core specification is designed based on L2 by default. If a binding layer cannot guarantee performance (e.g., differences in Python's `random` implementation), the documentation must explicitly declare a fallback.

## 5. Test Matrix (Must be all green for release)

### 5.1 Core Logic Testing (C Layer, the only authoritative)

| Test | Method | Pass Criteria |

|---|---|---|

| Frequency | 1e6 samplings, chi-square | Frequency deviation from `w_i/Σw` < ε |

| Cooling Violation | `L=1, beta_q=0` | Consecutive identical samplings = 0 |

| Coverage | `L≥n, equal weight` | Exactly covers the entire set every n samplings |

| Boundaries | `n=1`, `L=0`, `L>n`, `total=0` | No crashes, behavior conforms to the degenerate table |

| Unbiased Mapping | `n=3, 5, 7` each 1e6 samplings | Uniform frequency across indices |

| Reproducibility | Same seed, two initializations | Sampling sequence identical bit by bit |

### 5.2 Cross-Language Consistency Testing

For each language binding, run N times with **the same fixed seed and parameters**, outputting the SHA-256 of the index sequence, and compare it with the C reference output.

``text Test Vector (must pass):
seed=42, stream_id=0, n=10, w=all 1, L=3, beta_q=0, alpha_q=0, R=1000

SHA-256 of the first 100 output indices: <expected>

```

If a language cannot achieve L2 reproducibility, the language test is downgraded to "intra-language reproducibility," but this must be clearly stated in the README.

```text Test Vector (must pass):

seed=42, stream_id=0, n=10, w=all 1, L=3, beta_q=0, alpha_q=0, R=1000

SHA-256 of the first 100 output indices: <expected>

```text Test Vector (must pass):

seed=42, stream_id=0, n=10, w=all 1, L=3, beta_q=0, alpha_q=0, R=1000

SHA-256 of the first 100 output indices: ... ### 5.3 Binding Layer Boundary Testing

- Null pointer exception, illegal `n`, `w_i=0`, `beta_q > 2^32` → Return error code, no crash;

- The core returned error is an exception in Python, `WccsException` in C#, `Error` in Node.js, and `std::runtime_error` in C++;

- Concurrent calls to the same handle (if thread-unsafe) must be documented or locked.

### 5.4 Packaging Acceptance

| Language | Acceptance |

|---|---|

| C/C++ | `pkg-config --cflags --libs wccs` is available; CMake `find_package` is available |

| Rust | `cargo test` passes; `cargo build --release` produces `.a`/`.so`/`.dylib`/`.dll` |

| Python | `pip install wccs` followed by `import wccs`; `maturin build` wheel passes manylinux audit |

| C# | NuGet package passes `dotnet test` on `win-x64`, `linux-x64`, `osx-arm64`; Native AOT has no trim warnings |

| Node.js | Node LTS / Bun / Deno are run once each; `import` and `require` are tested in both CJS and ESM |

## 6. Deployment Pipeline (CI Design)

### 6.1 Building the Matrix

```text Platform: ubuntu-22.04, macos-
14, Windows 2022
Architecture: x86_64, aarch64 (macOS/Linux)
Languages: C (core), C++, Rust, Python 3.9–3.13, .NET 8/10, Node 20/22

```

### 6.2 Phased Pipeline

**Phase 1: Core Build and Testing** (Triggers all downstream components)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

```

**Phase 2: Language-Specific Builds** (Parallel)

- Python: `maturin build --release` (Linux uses manylinux container)

- C#: `dotnet pack`, native libraries are placed in `runtimes/` according to RID

- Node: `napi build --platform --release`, platform packages and root packages are packaged separately

- Rust: `cargo build --release` + `cbindgen` generates header files

**Phase 3: Cross-language consistency testing**

Download the C reference binary from Phase 1, run fixed test vectors for each language, and compare SHA-256.

**Phase 4: Release**

| Target | Command/Mechanism |

|---|---|

| PyPI | `maturin publish` (or `uv publish`) |

| NuGet | `dotnet nuget push` |

| npm | Root package + platform optional packages `npm publish` respectively |

| crates.io | `cargo publish` (if releasing a Rust crate) |

| C/C++ | GitHub Release with `.a`/`.so`/`.dylib`/`.dll`/`.h` |

**Version locking**: Major versions of all binding packages are aligned with the core ABI version. When `wccs_abi_version()` returns `1`, the `wccs` dependency in all language packs' `package.json`, `pyproject.toml`, `.csproj`, and `Cargo.toml` must resolve to ABI 1.

## 7. Review Checklist (Check each item before release)

- [ ] C ABI header files have version macros; enumerations are appended to but not modified.

- [ ] All exported functions have `catch_unwind` (Rust) or equivalent error boundaries; no panic crossover.

- [ ] Core-allocated memory is only freed by `wccs_free` / `wccs_free_string`; binding layer has zero `free`.

- [ ] All language binding injection RNGs meet the target reproducibility level; explicitly stated in the README.

- [ ] Unbiased mapping `random_below` is implemented in the core; binding layer does not generate its own index.

- [ ] Weights, factors, and Fenwick are all integers/fixed-point; no cross-language floating-point divergence.

- [ ] Python wheel passes manylinux audit.

- [ ] C# Native AOT has no trim/AOT warnings.

- [ ] Node native plugins external to bundles; WASM Rollback is loadable;

- [ ] Cross-language fixed test vector SHA-256 consistency (or degradation declaration is documented);

- [ ] Cooling, coverage, boundary, and frequency tests all pass;

- [ ] Error code table and exception mapping are consistent across all languages.

**In short:** The creation of the WCCS multi-platform library requires **70% of the effort to ensure the determinism of the C core and WCCS logic, and 30% to bind and package it across languages**. The thinner the binding layer, the better; any practice of copying algorithm logic to the binding layer is the source of cross-language inconsistencies and long-term maintenance burden.