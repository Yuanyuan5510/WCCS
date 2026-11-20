/* WASM fallback loader for Deno / Edge Workers / browsers.
 *
 * `wccs.mjs` is produced by build.sh (Emscripten, WASM_BIGINT=1) in CI;
 * it is deliberately not committed. The Node/Bun path uses index.cjs
 * (koffi FFI) instead. SPDX-License-Identifier: Apache-2.0
 */
import createWccsModule from './wccs.mjs';

const u64 = (v) => (typeof v === 'bigint' ? v : BigInt(v));

export class WccsError extends Error {
  constructor(code) {
    super(`wccs error ${code}`);
    this.name = 'WccsError';
    this.code = code;
  }
}

/** Instantiate the WASM module once and share it across samplers. */
export async function createWccsWasm() {
  return await createWccsModule();
}

export class WasmSampler {
  /** @param {ReturnType<typeof createWccsWasm>} M (await the promise first) */
  constructor(M, weights, options = {}) {
    const {
      L = 0, beta_q = 0, alpha_q = 0, R = 1, seed = 0, stream_id = 0,
    } = options;
    const n = weights.length;
    const wp = M._malloc(n * 8);
    for (let i = 0; i < n; i++) M.HEAPU64[wp / 8 + i] = u64(weights[i]);
    const ep = M._malloc(4);
    M.HEAP32[ep / 4] = 0;

    const ctx = M._wccs_new_default(
      n, wp, L, u64(beta_q), u64(alpha_q), R,
      u64(seed), u64(stream_id), ep,
    );
    const err = M.HEAP32[ep / 4];
    M._free(wp);
    M._free(ep);
    if (err !== 0 || !ctx) throw new WccsError(err || 2);

    this.M = M;
    this.ctx = ctx;
  }

  sample() {
    if (!this.ctx) throw new WccsError(5);
    const ep = this.M._malloc(4);
    this.M.HEAP32[ep / 4] = 0;
    const r = this.M._wccs_sample(this.ctx, ep);
    const err = this.M.HEAP32[ep / 4];
    this.M._free(ep);
    if (err !== 0) throw new WccsError(err);
    return r;
  }

  get step() { return this.ctx ? Number(this.M._wccs_step(this.ctx)) : 0; }
  get total() { return this.ctx ? Number(this.M._wccs_total(this.ctx)) : 0; }

  close() {
    if (this.ctx) { this.M._wccs_free(this.ctx); this.ctx = 0; }
  }
}
