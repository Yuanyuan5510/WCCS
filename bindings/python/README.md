# wccs (Python)

Python binding for [WCCS](https://github.com/Yuanyuan5510/WCCS), the
Weighted Credit-Cooldown Sampler.

This package is a thin `ctypes` layer over the versioned C ABI. It contains
**no algorithm logic** — all sampling happens in the C core. By default it
calls `wccs_new_default`, which injects the core's deterministic PCG64, so
the same seed produces the same sequence in every language (L2).

## Install

```bash
pip install wccs                # from PyPI: platform wheel, native library included
pip install ./bindings/python   # or from a source checkout
```

Release wheels bundle the native library inside the `wccs` package, so nothing
else is required. For a source checkout without a bundled library, build it
with CMake (`cmake -B build && cmake --build build`) or point the loader at an
existing build:

```bash
export WCCS_LIBRARY_PATH=/path/to/libwccs.so   # or .dll / .dylib
```

## Usage

```python
from wccs import Sampler

with Sampler([1, 2, 3, 4], L=3, beta_q=0, R=1000, seed=42) as s:
    for _ in range(10):
        print(s.sample())
    print(s.step, s.total)
```

Pass `rng=` (a callable returning a 64-bit integer) to exercise the C ABI's
random-source injection via `wccs_new`. Doing so gives up the L2 guarantee
unless every language injects the identical engine.

Errors are raised as `wccs.WccsError` with a stable `.code`.

## Tests

```bash
PYTHONPATH=. python -m unittest discover -s tests -v
PYTHONPATH=. python tests/vector.py     # cross-language vector emitter
```

License: Apache-2.0
