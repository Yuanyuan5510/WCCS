'use strict';
/* WCCS Node.js binding.
 *
 * Thin FFI layer over the C ABI: type mapping, handle ownership and
 * error-to-Error translation only. No algorithm logic is duplicated here.
 *
 * Runtime strategy (work.md section 3.5):
 *   - Node / Bun  -> native library via koffi (this file, zero compilation)
 *   - Deno / Edge / browser -> WASM fallback, see wasm/build.sh + wasm/loader.mjs
 *
 * SPDX-License-Identifier: Apache-2.0
 */
const fs = require('fs');
const path = require('path');
const koffi = require('koffi');

const ABI_VERSION = 1;

const ErrorCodes = Object.freeze({
  OK: 0, E_INVAL: 1, E_NOMEM: 2, E_OVERFLOW: 3,
  E_EMPTY: 4, E_NULL: 5, E_STATE: 6,
});

function findLibrary() {
  if (process.env.WCCS_LIBRARY_PATH) return process.env.WCCS_LIBRARY_PATH;
  const root = path.resolve(__dirname, '..', '..');
  const dirs = [
    __dirname,                              // native lib bundled in the tarball
    path.join(root, 'build'),
    path.join(root, 'build', 'Release'),
    path.join(root, 'build', 'Debug'),
    root,
  ];
  const names = ['libwccs.dll', 'wccs.dll', 'libwccs.so', 'libwccs.dylib'];
  for (const d of dirs) {
    for (const n of names) {
      const p = path.join(d, n);
      if (fs.existsSync(p)) return p;
    }
  }
  return 'wccs'; // let the OS loader resolve it from the system path
}

const lib = koffi.load(findLibrary());

// Callback prototype for the injectable random source (wccs_rng_t).
const RngProto = koffi.proto('WccsNextU64', 'uint64', ['void *']);
const RngPtr = koffi.pointer(RngProto);
const RngStruct = koffi.struct('wccs_rng_t', {
  ctx: 'void *',
  next_u64: RngPtr,
});

// err is an output parameter: koffi does not copy plain arrays back to JS
// unless the pointer is explicitly marked out/inout.
const OutInt = koffi.out(koffi.pointer('int'));

const fn = {
  newDefault: lib.func('wccs_new_default', 'void *',
    ['int', 'uint64 *', 'int', 'uint64', 'uint64', 'int', 'uint64', 'uint64', OutInt]),
  newInjected: lib.func('wccs_new', 'void *',
    ['int', 'uint64 *', 'int', 'uint64', 'uint64', 'int', 'uint64', 'uint64', RngStruct, OutInt]),
  sample: lib.func('wccs_sample', 'int', ['void *', OutInt]),
  free: lib.func('wccs_free', 'void', ['void *']),
  abiVersion: lib.func('wccs_abi_version', 'int', []),
  errorString: lib.func('wccs_error_string', 'str', ['int']),
  step: lib.func('wccs_step', 'uint64', ['void *']),
  total: lib.func('wccs_total', 'uint64', ['void *']),
};

class WccsError extends Error {
  constructor(code) {
    let message = 'unknown error';
    try { message = fn.errorString(code) || message; } catch (_) { /* keep default */ }
    super(`wccs error ${code}: ${message}`);
    this.name = 'WccsError';
    this.code = code;
  }
}

function asU64(value) {
  if (typeof value === 'bigint') return value & 0xffffffffffffffffn;
  if (typeof value === 'number') return BigInt(Math.trunc(value)) & 0xffffffffffffffffn;
  return BigInt(value) & 0xffffffffffffffffn;
}

class Sampler {
  /**
   * @param {Array<number|bigint>} weights positive base weights
   * @param {{L?:number,beta_q?:number|bigint,alpha_q?:number|bigint,
   *          R?:number,seed?:number|bigint,stream_id?:number|bigint,
   *          rng?:()=>number|bigint}} [options]
   */
  constructor(weights, options = {}) {
    const {
      L = 0, beta_q = 0, alpha_q = 0, R = 1,
      seed = 0, stream_id = 0, rng = null,
    } = options;

    const w = Array.from(weights, asU64);
    const n = w.length;
    const err = [0];
    this._ctx = null;
    this._callback = null;

    if (rng === null) {
      this._ctx = fn.newDefault(n, w, L, asU64(beta_q), asU64(alpha_q),
                                R, asU64(seed), asU64(stream_id), err);
    } else {
      const callback = () => asU64(rng());
      this._callback = koffi.register(callback, RngPtr);
      this._ctx = fn.newInjected(n, w, L, asU64(beta_q), asU64(alpha_q),
                                 R, asU64(seed), asU64(stream_id),
                                 { ctx: null, next_u64: this._callback }, err);
    }

    if (err[0] !== ErrorCodes.OK || !this._ctx) {
      const code = err[0] || ErrorCodes.E_NOMEM;
      this.close();
      throw new WccsError(code);
    }
  }

  sample() {
    if (!this._ctx) throw new WccsError(ErrorCodes.E_NULL);
    const err = [0];
    const r = fn.sample(this._ctx, err);
    if (err[0] !== ErrorCodes.OK) throw new WccsError(err[0]);
    return r;
  }

  get step() { return this._ctx ? Number(fn.step(this._ctx)) : 0; }
  get total() { return this._ctx ? Number(fn.total(this._ctx)) : 0; }

  close() {
    if (this._ctx) { fn.free(this._ctx); this._ctx = null; }
    if (this._callback !== null) {
      try { koffi.unregister(this._callback); } catch (_) { /* already released */ }
      this._callback = null;
    }
  }

  [Symbol.dispose]() { this.close(); } // `using s = new Sampler(...)` on Node 20+
}

function abiVersion() { return fn.abiVersion(); }

module.exports = { Sampler, WccsError, ErrorCodes, ABI_VERSION, abiVersion };
