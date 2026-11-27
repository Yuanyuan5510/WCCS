# Release review checklist (work.md §7)

Status as of this implementation. Re-run before every release.

- [x] C ABI header has a version macro; enums are append-only.
      `include/wccs.h`, `WCCS_ABI_VERSION 1`.
- [x] No panic/exception crosses the C ABI. Pure C core; errors returned via
      `int32_t *err` only. (Rust binding is a consumer, not the core.)
- [x] Core-allocated memory is released only by `wccs_free`; bindings contain
      zero `free` calls on core pointers.
- [x] All bindings inject the same deterministic PRNG by default
      (`wccs_new_default` → core PCG RXS-M-XS 64/64). Custom injection
      downgrades to L0/L1 and is documented in `docs/CONTRACT.md` §7.
- [x] [`docs/CONTRACT.md`](CONTRACT.md) is implemented and covered: cooldown
      reference counting (§2), soft-cooldown floor (§3), `total == 0`
      boundary exclusion (§4), optional stream-conflict detection (§5).
- [x] The `(seed, stream_id)` detector defaults to off, is append-only in the
      ABI (`WCCS_E_STREAM_CONFLICT = 7`), and never fails a valid
      construction just because the registry is full.
- [x] `random_below` (Lemire) lives in the core; no binding generates indices.
- [x] Weights, factors and Fenwick tree are integer / Q32 only; no float.
- [ ] Python wheel passes a `manylinux` audit — **not yet run**; CI builds
      with `maturin`/`auditwheel` are a release-stage task. The pure-`ctypes`
      package is platform-independent, but the native library still needs a
      manylinux build.
- [x] C# Native AOT has no trim/AOT warnings from the P/Invoke layer:
      `[LibraryImport]` (source-generated) is used throughout.
- [x] Node native path is external to any bundle; WASM fallback is loadable
      (`wasm/build.sh` + `wasm/loader.mjs`, built in CI).
- [x] Cross-language fixed vector SHA-256 matches
      (`tests/run_cross_lang.py`, 7/7 entry points).
- [x] Cooldown, coverage, boundary, frequency and unbiased-mapping tests pass
      (`ctest`, 48 802 checks, 0 failures).
- [x] Error codes and exception mappings are consistent across languages.

## Release pipeline status

The CI `package` job now produces, per platform: the C/C++ SDK, a Python wheel
with the native library bundled, an npm tarball, the C# assembly with its
native dependency, and a self-contained Rust crate. A `v*` tag publishes all
of them plus the WASM build to a GitHub Release.

| Item | Status |
|---|---|
| Version gate (`VERSION` == manifests == `v*` tag) | automated; mismatch fails the run before building |
| GitHub Release with all platform bundles + WASM | automated on a matching `v*` tag |
| PyPI wheels (`win_amd64`, `macosx_11_0_arm64`, `manylinux_<glibc>_x86_64` via PEP 600 tag) | automated in the package job |
| PyPI publication | automated on a matching `v*` tag when the `PYPI_TOKEN` secret is set (`skip-existing`, so re-runs are safe) |
| NuGet RID packaging (`runtimes/{rid}/native/`) | not done — CI ships a zip instead of a RID package |
| npm platform optional packages | not done — CI ships one tarball per platform |
| crates.io / npm / NuGet publication | manual, requires registry secrets |
