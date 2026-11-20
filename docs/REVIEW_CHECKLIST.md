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
      downgrades to L0/L1 and is documented in `docs/IMPLEMENTATION_NOTES.md`.
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
      (`ctest`, 48 793 checks, 0 failures).
- [x] Error codes and exception mappings are consistent across languages.

## Outstanding release-stage items

| Item | Owner task |
|---|---|
| `manylinux` wheel audit for the native library | publish pipeline |
| NuGet RID packaging (`runtimes/{rid}/native/`) | publish pipeline |
| npm platform optional packages (currently one FFI package + WASM build) | publish pipeline |
| crates.io / PyPI / npm / NuGet publication | manual, `workflow_dispatch` |
| WASM artifact attached to GitHub Release | CI `wasm` job |
