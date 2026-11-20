"""WCCS Python binding (ctypes / C ABI).

Thin translation layer only: parameter construction, handle ownership and
error-to-exception mapping. All WCCS logic lives in the C core.

By default ``Sampler`` uses ``wccs_new_default`` which injects the core's
reference PCG64, so the same seed produces the same sequence in every
language (L2 reproducibility). A custom ``next_u64`` callable can be supplied
via ``rng=`` to exercise the injection interface (wccs_new).

SPDX-License-Identifier: Apache-2.0
"""
from __future__ import annotations

import ctypes
import ctypes.util
import os
from pathlib import Path

__all__ = [
    "Sampler", "WccsError", "abi_version",
    "OK", "E_INVAL", "E_NOMEM", "E_OVERFLOW", "E_EMPTY", "E_NULL", "E_STATE",
]

OK = 0
E_INVAL = 1
E_NOMEM = 2
E_OVERFLOW = 3
E_EMPTY = 4
E_NULL = 5
E_STATE = 6

_u64 = ctypes.c_uint64
_i32 = ctypes.c_int32
_pu64 = ctypes.POINTER(_u64)
_pi32 = ctypes.POINTER(_i32)
_voidp = ctypes.c_void_p

_RNG_FUNCTYPE = ctypes.CFUNCTYPE(_u64, _voidp)


class _Rng(ctypes.Structure):
    _fields_ = [("ctx", _voidp), ("next_u64", _RNG_FUNCTYPE)]


def _library_candidates():
    env = os.environ.get("WCCS_LIBRARY_PATH")
    if env:
        yield Path(env)
    names = ("libwccs.dll", "wccs.dll", "libwccs.so", "libwccs.dylib")
    here = Path(__file__).resolve().parent

    # 1) native library bundled inside an installed wheel
    for n in names:
        yield here / n

    # 2) repository build directory (source checkout)
    parents = Path(__file__).resolve().parents
    if len(parents) > 3:
        root = parents[3]
        for d in (root / "build", root / "build" / "Release",
                  root / "build" / "Debug", root):
            for n in names:
                yield d / n

    found = ctypes.util.find_library("wccs")
    if found:
        yield Path(found)


def _load_library():
    errors = []
    for p in _library_candidates():
        try:
            return ctypes.CDLL(str(p))
        except OSError as exc:  # pragma: no cover - platform dependent
            errors.append(f"{p}: {exc}")
    # Last resort: let the dynamic loader search the system paths.
    try:
        return ctypes.CDLL("wccs")
    except OSError as exc:
        raise ImportError(
            "Cannot locate the WCCS C library. Build it with CMake or set "
            "WCCS_LIBRARY_PATH to libwccs.dll / .so / .dylib.\n"
            + "\n".join(errors)
        ) from exc


_lib = _load_library()

_lib.wccs_new.argtypes = [_i32, _pu64, _i32, _u64, _u64, _i32, _u64, _u64, _Rng, _pi32]
_lib.wccs_new.restype = _voidp

_lib.wccs_new_default.argtypes = [_i32, _pu64, _i32, _u64, _u64, _i32, _u64, _u64, _pi32]
_lib.wccs_new_default.restype = _voidp

_lib.wccs_sample.argtypes = [_voidp, _pi32]
_lib.wccs_sample.restype = _i32
_lib.wccs_free.argtypes = [_voidp]
_lib.wccs_free.restype = None
_lib.wccs_abi_version.argtypes = []
_lib.wccs_abi_version.restype = _i32
_lib.wccs_error_string.argtypes = [_i32]
_lib.wccs_error_string.restype = ctypes.c_char_p
_lib.wccs_step.argtypes = [_voidp]
_lib.wccs_step.restype = _u64
_lib.wccs_total.argtypes = [_voidp]
_lib.wccs_total.restype = _u64


class WccsError(Exception):
    """A stable WCCS error code raised as a Python exception."""

    def __init__(self, code: int):
        self.code = code
        raw = _lib.wccs_error_string(code)
        message = raw.decode("utf-8", "replace") if raw else "unknown error"
        super().__init__(f"wccs error {code}: {message}")


def abi_version() -> int:
    return int(_lib.wccs_abi_version())


class Sampler:
    """Owns one opaque WCCS context. A single handle is not thread-safe."""

    def __init__(
        self,
        weights,
        L: int = 0,
        beta_q: int = 0,
        alpha_q: int = 0,
        R: int = 1,
        seed: int = 0,
        stream_id: int = 0,
        rng=None,
    ):
        values = [int(x) for x in weights]
        arr = (_u64 * len(values))(*values)
        self._weights = arr            # keep the buffer alive
        self._rng_cb = None            # keep an injected callback alive
        self._ctx = None
        err = _i32(0)
        n = len(values)

        if rng is None:
            self._ctx = _lib.wccs_new_default(
                n, arr, L, beta_q, alpha_q, R, seed, stream_id, ctypes.byref(err)
            )
        else:
            def _next(_ctx):
                return int(rng()) & 0xFFFFFFFFFFFFFFFF

            cb = _RNG_FUNCTYPE(_next)
            self._rng_cb = cb
            rng_struct = _Rng(None, cb)
            self._ctx = _lib.wccs_new(
                n, arr, L, beta_q, alpha_q, R, seed, stream_id,
                rng_struct, ctypes.byref(err),
            )

        if err.value != OK or not self._ctx:
            self._ctx = None
            raise WccsError(err.value if err.value else E_NOMEM)

    def sample(self) -> int:
        if not self._ctx:
            raise WccsError(E_NULL)
        err = _i32(0)
        r = _lib.wccs_sample(self._ctx, ctypes.byref(err))
        if err.value != OK:
            raise WccsError(err.value)
        return int(r)

    @property
    def step(self) -> int:
        return int(_lib.wccs_step(self._ctx)) if self._ctx else 0

    @property
    def total(self) -> int:
        return int(_lib.wccs_total(self._ctx)) if self._ctx else 0

    def close(self):
        if self._ctx:
            _lib.wccs_free(self._ctx)
            self._ctx = None

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()

    def __del__(self):  # pragma: no cover - interpreter shutdown safety
        try:
            self.close()
        except Exception:
            pass
