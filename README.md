# WCCS — Weighted Credit-Cooldown Sampler

**WCCS** is a deterministic constraint-sampling algorithm: weighted sampling
with short-term cooldown, long-term frequency correction, and parallel
reproducibility on top of a high-quality random source.

This repository is a concrete implementation of the WCCS design manual —
[`work.md`](work.md) is the condensed English edition, and the original
Chinese long-form manual is kept as [`work.zh.md`](work.zh.md). It follows the
manual's central rule: **one C core, six thin bindings**. Algorithm logic is
written exactly once; every language only translates types, ownership and
errors.

```
                 include/wccs.h  (versioned C ABI, ABI 1)
                          |
                   src/wccs.c    (the only source of truth)
                          |
   +----------+----------+----------+----------+----------+----------+
   | C / C++  |  Rust    | Python   |   C#     | Node.js  |  WASM    |
   +----------+----------+----------+----------+----------+----------+
```

## Features

- **Weighted** sampling with a Fenwick tree over integer weights.
- **Short-term cooldown**: the last `L` picks are suppressed (`beta_q=0`) or
  softly dampened (`beta_q>0`).
- **Long-term credit correction**: every `R` samples, actual counts are
  pulled back towards their target frequencies.
- **Unbiased** bounded randomness (Lemire, no `% n` on the random value).
- **L2 reproducibility**: same `seed` + `stream_id` → byte-identical index
  sequence on every language and platform. No floating point anywhere.
- **Stable C ABI**: opaque handle, append-only error codes, no exceptions
  across the boundary.

## Algorithm

All state is integer / Q32 fixed point (see [`docs/ABI.md`](docs/ABI.md) and
[`docs/IMPLEMENTATION_NOTES.md`](docs/IMPLEMENTATION_NOTES.md)).

```
v_i = max(0, w_i + (alpha_q * credit_i >> 64)) * cooldown_i >> 32

sample:
  if total == 0: clear cooldown queue, rebuild Fenwick
  r = random_below(total)                    # Lemire, unbiased
  i = fenwick_find_by_cumulative(r)
  actual[i]++, step++
  cooldown[i] = beta_q                       # 0 = strict, 2^32 = off
  if window full: restore oldest
  push i into the length-L ring buffer
  if step % R == 0: rebuild_credit()

rebuild_credit:
  target_i = (w_i * step) / W_total
  credit_i = (target_i - actual_i) << 32     # Q32
```

| Config | Behaviour |
|---|---|
| `L=0, alpha_q=0` | pure weighted random, repeats allowed |
| `L=1, beta_q=0` | no consecutive repeats |
| `L>=n, beta_q=0`, equal weights | random permutation every `n` draws |
| `beta_q>0` | soft cooldown; high weights are not starved |
| `total=0` | clear cooldown, restore all factors, rebuild |

The reference PRNG is **PCG RXS-M-XS 64/64** (64-bit state, 64-bit output),
built into the core and exposed as `wccs_pcg64_*`.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

On Windows with MinGW use `-G "MinGW Makefiles"`. CMake produces:

- `libwccs.so` / `.dylib` / `wccs.dll` — shared C ABI library
- `libwccs_static.a` / `wccs_static.lib` — static library
- `wccs.pc` + `wccs-config.cmake` — pkg-config / `find_package(wccs)`

Install with `cmake -B build -DCMAKE_INSTALL_PREFIX=<dir> && cmake --install
build` (set the prefix at **configure** time — `wccs.pc` embeds it).

> Linking the **static** library requires `-DWCCS_STATIC` (the CMake target
> `wccs::wccs_static` propagates it automatically).

## Quick start by language

<details open>
<summary>C</summary>

```c
#include "wccs.h"
uint64_t w[4] = {1, 2, 3, 4};
int32_t err = 0;
wccs_ctx *c = wccs_new_default(4, w, 3, 0, 0, 1000, 42, 0, &err);
for (int i = 0; i < 10; i++) printf("%d\n", wccs_sample(c, &err));
wccs_free(c);
```
</details>

<details>
<summary>C++ (header-only RAII)</summary>

```cpp
#include "wccs.hpp"
wccs::Sampler s({1, 2, 3, 4}, /*L=*/3, 0, 0, /*R=*/1000, /*seed=*/42);
int idx = s.sample();          // throws wccs::Error on failure
```
</details>

<details>
<summary>Rust</summary>

```rust
use wccs::Sampler;
let w = [1u64, 2, 3, 4];
let mut s = Sampler::new(&w, 3, 0, 0, 1000, 42, 0)?;
let idx = s.sample()?;
```
`cargo test --manifest-path bindings/rust/Cargo.toml`
</details>

<details>
<summary>Python (ctypes)</summary>

```python
from wccs import Sampler
with Sampler([1, 2, 3, 4], L=3, R=1000, seed=42) as s:
    print(s.sample())
```
`pip install ./bindings/python` — set `WCCS_LIBRARY_PATH` if the native
library is not in `build/`.
</details>

<details>
<summary>C# (P/Invoke, Native AOT friendly)</summary>

```csharp
using Wccs;
using var s = new Sampler(new ulong[] {1, 2, 3, 4}, L: 3, R: 1000, seed: 42);
int idx = s.Sample();
```
`dotnet run --project bindings/csharp/Wccs.Tests -c Release`
</details>

<details>
<summary>Node.js (CJS + ESM)</summary>

```js
const { Sampler } = require('wccs');                 // CJS
// import { Sampler } from 'wccs';                  // ESM
const s = new Sampler([1, 2, 3, 4], { L: 3, R: 1000, seed: 42 });
const idx = s.sample();
s.close();
```
`cd bindings/node && npm ci && npm test`. Node/Bun use the native library
through `koffi`; Deno/Edge/browser use the WASM fallback in `wasm/`.
</details>

### Random source injection

`wccs_new` accepts a `wccs_rng_t { void *ctx; uint64_t (*next_u64)(void*) }`.
All bindings expose it (Python `rng=`, C# `rng:`, Node `rng`, Rust/C++ via
the raw ABI). Use `wccs_new_default` unless you deliberately want to supply
your own engine — see the reproducibility section below.

## Reproducibility

| Level | Guarantee | How |
|---|---|---|
| L0 | same language, same platform | fixed seed + host RNG |
| L1 | same language, cross-platform | integer/Q32 + unbiased mapping |
| **L2** | **cross-language, cross-platform** | L1 + one shared deterministic PRNG + zero float |

**This repository targets L2 by default** via `wccs_new_default`. Injecting a
custom RNG with `wccs_new` downgrades to L0/L1 unless every language injects
the identical engine; document such a downgrade explicitly.

Verified locally on Windows with **two different C compilers** (MinGW GCC 8.1
for the shared library, MSVC via Rust's `cc` crate for the static library) —
both produce the same sequence.

## Tests

C core test matrix (`work.md` §5.1), 48 793 assertions, 0 failures:

- reproducibility (2 000 draws × 2 contexts)
- cooldown violation (`L=1`, 20 000 draws)
- full coverage (`L=n`, 50 rounds)
- unbiased mapping (`n=3,5,7`, 10⁶ draws each, ±5%)
- weighted frequency (10⁶ draws, ±2%)
- boundaries (`n=1`, `L=0`, `L>n`, `total=0`, `beta_q=2^32`)
- error codes, credit-rebuild determinism, helpers

Cross-language fixed vector (`work.md` §5.2):

```text
seed=42, stream_id=0, n=10, w=all 1, L=3, beta_q=0, alpha_q=0, R=1000
SHA256(first 100 indices as 100 uint8 bytes) =
  30776469ff25ee678e297c43e0bdaadb352340f6b53fcb0587d091209f08ba68
```

```bash
python tests/run_cross_lang.py
```

| Entry point | Result |
|---|---|
| C (`ctest`) | ✅ matches |
| C++ | ✅ matches |
| Rust | ✅ matches |
| Python | ✅ matches |
| C# | ✅ matches |
| Node CJS | ✅ matches |
| Node ESM | ✅ matches |

Per-language suites: `ctest`, `cargo test`, `python -m unittest discover -s
bindings/python/tests`, `dotnet run --project bindings/csharp/Wccs.Tests`,
`npm test`.

## CI & release

`.github/workflows/ci.yml` implements the five stages of `work.md` §6:

1. **core** — Ubuntu 22.04 / macOS 15 / Windows 2022, CMake + `ctest`
2. **bindings** — same matrix, all six languages + `run_cross_lang.py`
3. **wasm** — Emscripten build of the C core, uploaded as an artifact
4. **package** — runs on every push to `main`, on `v*` tags and on manual
   dispatch (PRs skip it) and builds:
   - C/C++ SDK (`cmake --install` layout: lib, headers, `wccs.pc`, CMake config)
   - Python wheel with the native library bundled next to `wccs/__init__.py`
   - npm tarball with the native library bundled
   - C# assembly plus its native dependency
   - self-contained Rust crate (C source vendored under `vendor/`)
5. **release** — on a `v*` tag, publishes every platform bundle and the WASM
   output to a GitHub Release.

Cut a release with:

```bash
git tag -a v1.0.0 -m "WCCS 1.0.0 (C ABI 1)"
git push origin v1.0.0
```

Uploading to PyPI / NuGet / npm / crates.io still needs registry tokens and is
a manual step; see [`docs/REVIEW_CHECKLIST.md`](docs/REVIEW_CHECKLIST.md).

## Repository layout

```
include/wccs.h              versioned C ABI
src/wccs.c                  the only algorithm implementation
CMakeLists.txt              core + C++ test + install/packaging
tests/                      C test matrix, SHA-256 tool, cross-language runner
tests/vectors/v1.txt        pinned reference vector
bindings/cpp/               header-only RAII
bindings/rust/              FFI + safe wrapper (builds C from source)
bindings/python/            ctypes package
bindings/csharp/            [LibraryImport] P/Invoke
bindings/node/              koffi FFI (CJS+ESM) + WASM fallback
docs/                       ABI, implementation notes, review checklist
work.md                     design manual (English, condensed)
work.zh.md                  original Chinese manual (long form, reference)
```

## Constraints

- One `wccs_ctx` is **not thread-safe**; use one per thread.
- `Σ w_i` and `Σ v_i` must fit in `uint64` (`WCCS_E_OVERFLOW`).
- WASM fallback requires Emscripten; generated files are CI artifacts.
- Design decisions that differ from `work.md` pseudocode are documented in
  [`docs/IMPLEMENTATION_NOTES.md`](docs/IMPLEMENTATION_NOTES.md).

## License

Apache-2.0. See [`LICENSE`](LICENSE).
