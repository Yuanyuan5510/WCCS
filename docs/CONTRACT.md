# WCCS behaviour contract

> **Status: authoritative.** This appendix is the ruling for every point that
> `work.md` / `work.zh.md` leave undefined, or where the manual contradicts its
> own degeneracy table. It is not a changelog and not a rationale for "why the
> manual was changed" — it is the definition a conforming implementation must
> implement.

## 0. Precedence

| Layer | Document | Role |
|---|---|---|
| 1 | [`ABI.md`](ABI.md) + `include/wccs.h` | stable C ABI surface, append-only |
| 2 | **this file** | authoritative behaviour where the manual is silent or inconsistent |
| 3 | [`work.md`](../work.md) / [`work.zh.md`](../work.zh.md) | design intent and background |
| 4 | [`README.md`](../README.md) | summary |

On any conflict, the higher layer wins. **All binding-level test vectors are
governed by section 7 of this document**; `tests/vectors/v1.txt` is the
executable snapshot of that section. A binding that deviates from it is
non-conforming even if its output matches an older manual example.

The single C core (`src/wccs.c`) is the only implementation of this contract.
No binding may reimplement any part of it.

## 1. State

```text
n, L, R, beta_q, alpha_q, seed, stream_id   constructor arguments
w[n]        base weights, copied, all > 0
bit[], cur[], total                        Fenwick tree over dynamic v_i
actual[n]   observed pick counts
credit[n]   Q32 signed credit
cooldown[n] Q32 factor: 0 = strict, 2^32 = available/off
cool_count[n] occurrences of each index in the cooldown window (section 2)
recent[L]   ring buffer of the last L draws
last_index  previous draw, -1 before the first
step        total number of draws
```

`beta_q` and `alpha_q` are Q32 in `[0, 2^32]` inclusive. `L >= 0`, `R >= 1`.

**32-bit targets.** Every array is `n` elements wide, so the constructor
requires `n * sizeof(uint64_t) <= SIZE_MAX - sizeof(uint64_t)` and
`L * sizeof(int32_t) <= SIZE_MAX`. When either would overflow a 32-bit
`size_t`, construction fails with `WCCS_E_NOMEM` rather than allocating a
short buffer. The algorithm itself is width-independent: all arithmetic is
`uint64`/`int64` and the 128-bit helpers are portable C99.

## 2. Cooldown window release semantics

The window is the **multiset** of the last `L` drawn indices. `cool_count[j]`
is the multiplicity of `j` in that multiset. This is the ruling for
`work.md` section 1.4, whose pseudocode cannot express repeated indices.

On each draw with `L > 0`, in this order:

1. If the window is full (`recent_count == L`), release the oldest slot `j`:
   decrement `recent_count`, decrement `cool_count[j]`, and **only if
   `cool_count[j] == 0`** set `cooldown[j] = 2^32` and refresh its Fenwick
   weight. If a newer occurrence of `j` is still in the window, `cooldown[j]`
   must stay at `beta_q`.
2. Set `cooldown[i] = beta_q`, refresh `v_i`, increment `cool_count[i]`, and
   write `i` into the ring buffer.

**Invariant (checkable):** `cooldown[j] == beta_q` iff `cool_count[j] > 0`,
otherwise `cooldown[j] == 2^32`. Violating it silently shortens the effective
cooldown and inflates short-gap repeats.

Consequences:

- With `beta_q = 0`, a cooled index has `v_i = 0`, so it cannot be drawn;
  the window therefore contains distinct indices, `cool_count` is 0 or 1,
  and step 1 reduces to the historical behaviour. `L = 1` forbids
  consecutive repeats. `L = 0` bypasses the cooldown path entirely,
  including `beta_q`.
- With `0 < beta_q < 2^32`, an index may be drawn while still cooled, so
  the window legitimately contains duplicates. Reference counting is what
  makes soft cooldown well-defined in that case.
- `beta_q = 2^32` turns cooldown off; window bookkeeping is harmless but has
  no effect on any `v_i`.

## 3. Fixed-point reduction and the soft-cooldown floor

```text
delta   = (alpha_q * credit_i) >> 64        # Q32 * Q32 = Q64 -> integer
base    = max(0, w_i + delta)
v_i     = (base * cooldown_i) >> 32         # Q32 factor
```

`credit_i` is Q32 (`(target_i - actual_i) << 32`) and `alpha_q` is Q32, so
their product is **Q64** and must be reduced by 64, not 32. `work.md` section
1.2 writes `>> 32`; that mixes a Q64 quantity with an integer weight and is
ruled out. All products use the portable 128-bit helpers (no `__int128`, no
floating point).

**Floor rule (ruling for `work.md` section 1.6 "soft cooldown"):**

```text
if base > 0 and 0 < cooldown_i < 2^32 and v_i == 0:  v_i = 1
```

`>> 32` truncates. Without the floor, `(1 * beta_q) >> 32 == 0` for every
`beta_q < 2^32`, i.e. a *soft* cooldown silently becomes a *hard* one for
small integer weights. The floor applies only to a strictly positive soft
factor: `beta_q = 0` must remain `v_i = 0` (strict), and `beta_q = 2^32`
must remain `v_i = base` (off), so neither receives a floor. The resolution
of `beta_q` is still Q32 — weights below `2^32` are quantised, which is a
documented limitation (section 6), not a rounding bug.

Reference values of `(w * beta_q) >> 32` before the floor:

| `w` | beta 0 | .25 | .5 | .9 | 1.0 |
|---:|---:|---:|---:|---:|---:|
| 1 | 0 | 0 | 0 | 0 | 1 |
| 2 | 0 | 0 | 1 | 1 | 2 |
| 10 | 0 | 2 | 5 | 8 | 10 |
| 100 | 0 | 25 | 50 | 89 | 100 |
| 1000 | 0 | 250 | 500 | 899 | 1000 |

## 4. The `total == 0` state machine

`total == 0` means every `v_i` is zero. With `beta_q = 0` and `L >= n` this is
unavoidable: only `n` distinct indices exist, so "the last `L` draws are all
distinct" is **physically unsatisfiable**. The manual does not define the
recovery. This section is the ruling.

On entry to `wccs_sample` with `total == 0`:

1. Set `cooldown[k] = 2^32` and `cool_count[k] = 0` for all `k`.
2. Reset `recent_head = recent_count = 0`.
3. Rebuild the Fenwick tree from `calc_v`. If `total` is still 0, return
   `WCCS_E_EMPTY`.
4. **Round-boundary exclusion:** if `n > 1` and a previous draw exists,
   temporarily set `v_last = 0` (and subtract it from `total`), draw once
   from the remaining elements, then restore `v_last`. A Fenwick cumulative
   search skips zero-weight elements, so this is an unbiased draw over the
   other `n-1` elements and needs no extra randomness beyond the normal
   draw.

Rationale: step 4 removes the consecutive repeat that a bare reset produces
at the boundary between two rounds. It preserves everything else the
degeneracy table promises.

**Resulting guarantee for `L >= n`, `beta_q = 0`, equal weights:**

- each block of `n` draws is still a random permutation (verified by
  `t_coverage` in `tests/test_core.c`);
- no consecutive repeats, for any `L` — the `L = 1` guarantee never
  degrades when `L` grows;
- repeats at distances `2..L` are **expected and permitted**: they are the
  mathematical consequence of `L >= n`, not a contract violation. A window
  length larger than the population cannot forbid them.

The old behaviour (release the whole window and immediately redraw from all
`n` elements) is ruled out: it produced `min_gap = 1` and made a larger `L`
produce *nearer* repeats.

## 5. Parallel `stream_id` conflicts

One `wccs_ctx` is **not thread-safe**. Parallel use requires one context per
thread and **a distinct `stream_id` per context**. Two live contexts with the
same `(seed, stream_id)` produce byte-identical sequences — intentional for
L2 reproducibility tests, a silent error in production.

An optional process-global detector is therefore provided (ABI 1 additions):

```c
int32_t wccs_stream_check(int32_t mode);   /* 0 off, 1 warn, 2 error; returns previous */
int32_t wccs_stream_check_mode(void);
```

| mode | on duplicate key while both contexts are live |
|---:|---|
| 0 (default) | no check; identical sequences are allowed |
| 1 | warning on `stderr`, construction succeeds |
| 2 | construction fails with `WCCS_E_STREAM_CONFLICT` (7) |

Rules:

- The check is **off by default** and must be toggled before any context is
  created; toggling itself is not thread-safe.
- The registry covers both `wccs_new` and `wccs_new_default`, holds 256 slots,
  and is guarded by a compare-and-swap spin lock. On platforms without atomic
  builtins it degrades to a single-threaded fallback suitable for debugging.
- A full registry only warns; a valid construction is never failed for lack
  of bookkeeping space.
- **Serial reuse is always legal**: `wccs_free` unregisters the key, so
  creating a new context with the same `(seed, stream_id)` after freeing the
  previous one never conflicts.
- Enabling mode 2 is incompatible with tests that deliberately keep two
  same-key contexts alive (for example `t_reproducibility`). Keep the check
  off for those, or use distinct `stream_id`s.

## 6. Known limitations (not contract violations)

These are properties of the design, not defects; they are listed so that a
reviewer does not mistake them for regressions.

- **Strict cooldown changes long-run frequencies.** With `beta_q = 0`, the
  marginal frequency is not `w_i / Σw`: recently drawn (typically
  high-weight) indices are excluded, so low weights are over-represented.
  Credit correction (`alpha_q`, `R`) pulls frequencies back but leaves a
  residual deviation. `work.md` section 5.1's chi-square test applies to
  `L = 0` only.
- **Credit rebuild is periodic.** `rebuild_credit` runs when
  `step % R == 0`; with the recommended `R = 1000`, a 500-draw sequence never
  rebuilds. Short sequences therefore see no credit correction at all.
- **Credit deltas are integer-truncated.** For very small `w_i / Σw`, the
  target count within one rebuild period can round to zero, so `alpha_q`
  cannot compensate extreme weight ratios (e.g. 1000:1) and low-weight
  elements may be starved.
- **Q32 quantisation.** `cooldown` and `alpha` have `2^-32` resolution; small
  integer weights are coarsely quantised (section 3).
- `Σ w_i` and `Σ v_i` must fit in `uint64`.
- On a 32-bit build the address space bounds `n` far below `INT32_MAX`
  (about 536 million elements before the `size_t` guard fires), and the
  process cannot actually back such an allocation.
- A single context is not thread-safe; no internal locking is provided.

## 7. Test vectors and reproducibility

The pinned L2 vector is defined here and **all seven binding entry points
must reproduce it**:

```text
seed = 42, stream_id = 0, n = 10, w = all 1,
L = 3, beta_q = 0, alpha_q = 0, R = 1000
SHA-256 of the first 100 indices (100 consecutive uint8 bytes):
30776469ff25ee678e297c43e0bdaadb352340f6b53fcb0587d091209f08ba68
```

`tests/vectors/v1.txt` stores the same 100 indices and digest.
`tests/run_cross_lang.py` compares every binding against this file.

The vector configuration uses `beta_q = 0` and `L < n`, so none of the
section 2 reference-counting path, section 3 floor, or section 4 reset path
is entered: the contract changes in v1.0.2 **leave this digest unchanged**.
That is deliberate — it keeps every already-published binding test valid.

Contract regression coverage:

| Section | Check |
|---|---|
| 2 | `cooldown[j] == beta_q` iff `cool_count[j] > 0`, sampled each draw |
| 3 | soft cooldown never truncates a positive base to `v_i = 0` |
| 4 | `L >= n` has zero consecutive repeats and each `n`-block is a permutation |
| 5 | duplicate-key detection, serial reuse, distinct keys |
| 6 | frequency under strict cooldown is documented, not asserted as `w_i/Σw` |

`tests/probe_duplicates.c` implements sections 2–4 and 6; it includes
`src/wccs.c` directly to observe internal state. `tests/test_core.c` covers
sections 4 and 5 through the public API.

L2 requires every binding to call `wccs_new_default` (the core PCG
RXS-M-XS 64/64). Injecting a host RNG through `wccs_new` downgrades to
L0/L1 and must be documented.

## 8. Versioning

ABI 1 additions in v1.0.2 (append-only, no existing signature or enum value
changed): `WCCS_E_STREAM_CONFLICT = 7`, `wccs_stream_check`,
`wccs_stream_check_mode`. The behaviour rulings in sections 2–4 are likewise
part of ABI 1's behavioural contract.

Any future behaviour change **must** in the same change:

1. update this file;
2. recompute `tests/vectors/v1.txt` if the pinned vector is affected;
3. update the digest in `README.md`, `tests/test_vector.c` and every binding
   test;
4. bump `VERSION` and all six package manifests together (see `ABI.md`).
