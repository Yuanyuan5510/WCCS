# wccs

Rust binding for **WCCS** (Weighted Credit-Cooldown Sampler): deterministic
weighted sampling with a short-term cooldown and long-term frequency
correction.

- Thin safe wrapper over the versioned C ABI. All algorithm logic lives in the
  C core, which `build.rs` compiles from vendored source — no prebuilt
  binaries and no CRT mismatch.
- Integer / Q32 only. The same `(seed, stream_id)` produces a byte-identical
  index sequence on every platform (L2 reproducibility).
- No `rand` dependency: the reference PCG64 engine is built into the core.

## Install

```toml
[dependencies]
wccs = "1.0"
```

## Usage

```rust
use wccs::Sampler;

let weights = [1u64; 10];
let mut s = Sampler::new(
    &weights,
    1,      // L: cooldown window
    0,      // beta_q: 0 = strict (no consecutive repeats)
    0,      // alpha_q: credit correction strength
    1000,   // R: credit rebuild period, in samples
    42,     // seed
    0,      // stream_id
).expect("init");

for _ in 0..5 {
    println!("{}", s.sample().expect("sample"));
}
```

`beta_q` and `alpha_q` are Q32 fixed point in `[0, 2^32]`: `0` is strict
cooldown, `1u64 << 32` disables it, and values in between damp the recent
picks instead of removing them.

A sampler is neither `Send` nor `Sync`. Use one per thread and give each
thread a distinct `stream_id`; two live samplers sharing `(seed, stream_id)`
produce identical sequences by design.

## Reproducibility

The pinned L2 vector shared by all six bindings:

```text
seed=42, stream_id=0, n=10, w=all 1, L=3, beta_q=0, alpha_q=0, R=1000
SHA-256 of the first 100 indices =
30776469ff25ee678e297c43e0bdaadb352340f6b53fcb0587d091209f08ba68
```

```bash
cargo run --example vector
```

## Behaviour and limits

The crate follows the project's
[behaviour contract](https://github.com/Yuanyuan5510/WCCS/blob/main/docs/CONTRACT.md),
which rules on cases the design manual leaves undefined (cooldown window
release, fixed-point reduction, the `total == 0` reset) and lists the known
limitations:

- strict cooldown shifts long-run frequencies away from `w_i / Σw`;
- credit rebuild is periodic, so short sequences may see no correction;
- with `L >= n` the window cannot forbid repeats at distances `2..L`
  (consecutive repeats are still impossible).

## Links

- Repository: <https://github.com/Yuanyuan5510/WCCS>
- C ABI: <https://github.com/Yuanyuan5510/WCCS/blob/main/docs/ABI.md>
- License: Apache-2.0
