/* ESM vector test - verifies the `import` path of package.json exports.
 * SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import { Sampler, ABI_VERSION } from '../index.mjs';

assert.equal(ABI_VERSION, 1);
const w = Array(10).fill(1);
const s = new Sampler(w, { L: 3, R: 1000, seed: 42 });
const idx = [];
for (let i = 0; i < 100; i++) idx.push(s.sample());
console.log('indices:', idx.join(' '));
s.close();
