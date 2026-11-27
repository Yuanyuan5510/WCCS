/* Type declarations for the WCCS Node.js binding.
 * SPDX-License-Identifier: Apache-2.0 */
export interface SamplerOptions {
  L?: number;
  beta_q?: number | bigint;
  alpha_q?: number | bigint;
  R?: number;
  seed?: number | bigint;
  stream_id?: number | bigint;
  rng?: () => number | bigint;
}

export class WccsError extends Error {
  code: number;
  constructor(code: number);
}

export class Sampler {
  constructor(weights: Array<number | bigint>, options?: SamplerOptions);
  sample(): number;
  readonly step: number;
  readonly total: number;
  close(): void;
  [Symbol.dispose](): void;
}

export const ErrorCodes: {
  readonly OK: 0; readonly E_INVAL: 1; readonly E_NOMEM: 2;
  readonly E_OVERFLOW: 3; readonly E_EMPTY: 4;
  readonly E_NULL: 5; readonly E_STATE: 6; readonly E_STREAM_CONFLICT: 7;
};
export const ABI_VERSION: 1;
export function abiVersion(): number;
