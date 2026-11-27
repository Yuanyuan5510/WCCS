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

All state is integer / Q32 fixed point. [`docs/ABI.md`](docs/ABI.md) defines
the surface; [`docs/CONTRACT.md`](docs/CONTRACT.md) is the authoritative ruling
for every case the manual leaves undefined (cooldown release, fixed-point
reduction, `total == 0`, parallel streams).

```
v_i = max(0, w_i + ((alpha_q * credit_i) >> 64)) * cooldown_i >> 32
      # floor to 1 when base > 0 and 0 < cooldown_i < 2^32   (CONTRACT §3)

sample:
  if total == 0:                           # every element is cooled
      reset cooldown/cool_count/Fenwick
      exclude last_index from this draw    # no repeat across a round boundary
  r = random_below(total)                  # Lemire, unbiased
  i = fenwick_find_by_cumulative(r)
  actual[i]++, step++, last_index = i
  if window full: release oldest slot j    # CONTRACT §2
      cool_count[j]--
      if cool_count[j] == 0: restore cooldown[j] and its weight
  cooldown[i] = beta_q                     # 0 = strict, 2^32 = off
  cool_count[i]++
  push i into the length-L ring buffer
  if step % R == 0: rebuild_credit()

rebuild_credit:
  target_i = (w_i * step) / W_total
  credit_i = (target_i - actual_i) << 32   # Q32
```

| Config | Behaviour |
|---|---|
| `L=0, alpha_q=0` | pure weighted random, repeats allowed |
| `L=1, beta_q=0` | no consecutive repeats |
| `L>=n, beta_q=0`, equal weights | random permutation every `n` draws |
| `beta_q>0` | soft cooldown; a cooled index keeps a non-zero floor |
| `total=0` | release the window, exclude the previous index, rebuild |

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

## Platforms

| Target | Native library | Source |
|---|---|---|
| `linux-x86_64`, `linux-aarch64` | `.so` | CMake / manylinux wheel |
| `linux-i686` | `.so` | `-m32` multilib, `manylinux_*_i686` wheel |
| `win32-x64` | `.dll` | CMake (MSVC or MinGW) |
| `win32-ia32` | `.dll` | MinGW i686 cross-build, `win32` wheel |
| `darwin-arm64` | `.dylib` | CMake / macOS wheel |
| `wasm32` | `.wasm` | Emscripten fallback |

32-bit targets are first-class: on `size_t == 32` the constructor rejects an
`n` whose `n * sizeof(uint64_t)` would wrap, returning `WCCS_E_NOMEM` instead
of under-allocating. The L2 fixed vector is identical on 32-bit and 64-bit;
CI asserts it with `gcc -m32` (i686) and MSVC `-A Win32` (x86).

## Quick start by language

### Install from package registries

```bash
dotnet add package Wccs --version 1.0.2
cargo add wccs
npm i @yuanyuan5510/wccs
pip install wccs
```

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
const { Sampler } = require('@yuanyuan5510/wccs');  // CJS
// import { Sampler } from '@yuanyuan5510/wccs';   // ESM
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

C core test matrix (`work.md` §5.1), 48 802 assertions, 0 failures:

- reproducibility (2 000 draws × 2 contexts)
- cooldown violation (`L=1`, 20 000 draws)
- full coverage (`L=n`, 50 rounds)
- unbiased mapping (`n=3,5,7`, 10⁶ draws each, ±5%)
- weighted frequency (10⁶ draws, ±2%)
- boundaries (`n=1`, `L=0`, `L>n`, `total=0`, `beta_q=2^32`)
- error codes, credit-rebuild determinism, helpers
- stream-conflict detector (duplicate key, serial reuse, distinct keys)
- 32-bit capacity guard (`size_t` overflow rejected, not truncated)

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

`.github/workflows/ci.yml` runs a version gate plus five stages:

0. **version gate** — [`VERSION`](VERSION) is the single source of truth.
   CI checks that `bindings/python/pyproject.toml`,
   `bindings/rust/Cargo.toml` and `bindings/node/package.json` all match it.
   On a tag it additionally requires `v<tag> == VERSION`; if they disagree the
   job fails and **nothing is built or published**. (`CMakeLists.txt` reads
   `VERSION` directly, so the C/C++ SDK cannot drift.)
1. **core** — Ubuntu 22.04 / macOS 15 / Windows 2022, CMake + `ctest`
2. **core32** — i686 Linux (`-m32` with multilib) and x86 Windows (`-A Win32`);
   the same `ctest` matrix runs on 32-bit and must match the L2 vector
3. **bindings** — same 64-bit matrix, all six languages + `run_cross_lang.py`
4. **wasm** — Emscripten build of the C core, uploaded as an artifact
5. **package** — only on a matching `v*` tag or a manual run. Builds:
   - C/C++ SDK (`cmake --install` layout: lib, headers, `wccs.pc`, CMake config)
   - Python wheel with the native library bundled next to `wccs/__init__.py`
   - npm root tarball plus one platform package per OS
     (`@yuanyuan5510/wccs-win32-x64`, `-win32-ia32`, `-linux-x64-gnu`,
     `-linux-ia32`, `-darwin-arm64`)
   - C# assembly plus its native dependency
   - self-contained Rust crate (C source vendored under `vendor/`)
6. **package-32bit** — Windows x86 and Linux i686 libraries built on Ubuntu
   (MinGW cross-compile + `-m32`), with their `win32` / `manylinux_i686`
   wheels, npm `ia32` packages and NuGet `win-x86` / `linux-x86` runtimes
7. **release** — on a matching `v*` tag, publishes every platform bundle and
   the WASM output to a GitHub Release.
8. **publish** — four idempotent jobs (PyPI, npm, NuGet, crates.io) run on the
   same tag and read their tokens from the `release` environment.

A plain push to `main` stops after the tests, so day-to-day commits stay fast.

Cut a release with:

```bash
# 1. bump VERSION and the three package manifests to the same value
# 2. commit, then tag with that exact version
git tag -a v1.0.2 -m "WCCS 1.0.2 (C ABI 1)"
git push origin main v1.0.2
```

Registry publication is automated but idempotent: every publish job skips a
version that already exists, so a tag can be re-run safely.

Two registry behaviours are worth knowing before deleting a published release:

- **PyPI** permanently reserves a filename once it has been uploaded. If a
  release is deleted, re-uploading the same wheel fails with `400 This filename
  was previously used`. Bump `VERSION`, or pass `WHEEL_BUILD=1` to `package` to
  emit `wccs-<version>-1-<tag>.whl`.
- **npm** blocks the name of a deleted package for 24 hours, regardless of
  version. Platform packages therefore use scoped names, which are also exempt
  from npm's "name too similar" rule that rejects unscoped `wccs`.

## Repository layout

```
VERSION                     single source of truth for the release version
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
docs/                       ABI, behaviour contract, review checklist
single_header/              amalgamated single-header build (no build system)
single_source/              amalgamated single-source build (no build system)
tools/                      amalgamation generator (keeps the core single-source)
work.md                     design manual (English, condensed)
work.zh.md                  original Chinese manual (long form, reference)
```

## Single-file drop-in

Two amalgamated builds are checked in for embedding and for review. Both are
generated from `include/wccs.h` + `src/wccs.c` by `python tools/amalgamate.py`
and behave identically to the CMake build.

```c
/* header-only: define the macro in exactly one translation unit */
#define WCCS_IMPLEMENTATION
#include "single_header/wccs.h"
```

```bash
# or compile the self-contained translation unit
cc -O2 single_source/wccs.c your_app.c
```

No CMake, generated headers, or dependencies beyond the C standard library.

## Constraints

- One `wccs_ctx` is **not thread-safe**; use one per thread with a distinct
  `stream_id`. `wccs_stream_check(2)` turns duplicate live keys into
  `WCCS_E_STREAM_CONFLICT` (off by default).
- `Σ w_i` and `Σ v_i` must fit in `uint64` (`WCCS_E_OVERFLOW`).
- WASM fallback requires Emscripten; generated files are CI artifacts.
- Rulings that resolve `work.md` ambiguities live in
  [`docs/CONTRACT.md`](docs/CONTRACT.md), together with the known limitations
  (strict-cooldown frequency shift, periodic credit rebuild, Q32 quantisation).

## License

Apache-2.0. See [`LICENSE`](LICENSE).
