/* ESM entry point for the WCCS Node.js binding.
 * SPDX-License-Identifier: Apache-2.0 */
import cjs from './index.cjs';

export const Sampler = cjs.Sampler;
export const WccsError = cjs.WccsError;
export const ErrorCodes = cjs.ErrorCodes;
export const ABI_VERSION = cjs.ABI_VERSION;
export const abiVersion = cjs.abiVersion;
export default cjs;
