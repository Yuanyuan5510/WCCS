/* CJS vector test. Output line "indices: ..." is consumed by run_cross_lang.py.
 * SPDX-License-Identifier: Apache-2.0 */
'use strict';
const assert = require('assert');
const { Sampler, abiVersion, WccsError, ErrorCodes } = require('../index.cjs');

assert.strictEqual(abiVersion(), 1, 'ABI version');

const w = Array(10).fill(1);
const s = new Sampler(w, { L: 3, R: 1000, seed: 42 });
assert.strictEqual(s.step, 0);
assert.strictEqual(s.total, 10);

const idx = [];
for (let i = 0; i < 100; i++) {
  const r = s.sample();
  assert(r >= 0 && r < 10, 'index in range');
  idx.push(r);
}
assert.strictEqual(s.step, 100);
console.log('indices:', idx.join(' '));

// Error translation
assert.throws(() => new Sampler([1, 0, 1]), (e) =>
  e instanceof WccsError && e.code === ErrorCodes.E_INVAL);

// Reproducibility
const a = new Sampler(w, { L: 3, R: 1000, seed: 42 });
const b = new Sampler(w, { L: 3, R: 1000, seed: 42 });
for (let i = 0; i < 500; i++) assert.strictEqual(a.sample(), b.sample());

// Injected RNG (wccs_new)
function makeRng() {
  let x = 0x12345678n;
  return () => { x = (x * 6364136223846793005n + 1n) & 0xffffffffffffffffn; return x; };
}
const c = new Sampler(w, { L: 3, R: 1000, seed: 1, rng: makeRng() });
const d = new Sampler(w, { L: 3, R: 1000, seed: 1, rng: makeRng() });
for (let i = 0; i < 100; i++) assert.strictEqual(c.sample(), d.sample());

[a, b, c, d, s].forEach((x) => x.close());
