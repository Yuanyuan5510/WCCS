"""Python binding unit tests (stdlib unittest, no third-party deps).
Run: PYTHONPATH=. python -m unittest discover -s tests -v
SPDX-License-Identifier: Apache-2.0"""
import unittest

from wccs import (Sampler, WccsError, abi_version,
                  E_INVAL, E_NULL, OK)


class TestWccs(unittest.TestCase):
    def test_abi_version(self):
        self.assertEqual(abi_version(), 1)

    def test_state_helpers(self):
        with Sampler([1, 2, 3, 4]) as s:
            self.assertEqual(s.step, 0)
            self.assertEqual(s.total, 10)
            self.assertIn(s.sample(), range(4))
            self.assertEqual(s.step, 1)

    def test_reproducible(self):
        a = Sampler([1] * 10, L=3, R=1000, seed=42)
        b = Sampler([1] * 10, L=3, R=1000, seed=42)
        self.assertEqual([a.sample() for _ in range(500)],
                         [b.sample() for _ in range(500)])
        a.close(); b.close()

    def test_invalid_input_raises(self):
        with self.assertRaises(WccsError) as cm:
            Sampler([1, 0, 1])
        self.assertEqual(cm.exception.code, E_INVAL)

    def test_use_after_close_raises(self):
        s = Sampler([1, 2])
        s.close()
        with self.assertRaises(WccsError) as cm:
            s.sample()
        self.assertEqual(cm.exception.code, E_NULL)

    def test_custom_rng_injection(self):
        # Injected RNG is deterministic and exercised through wccs_new.
        # Each sampler needs its own state object, otherwise the two
        # callbacks would advance one shared counter.
        def make_rng():
            state = {"x": 0x12345678}

            def next_u64():
                state["x"] = (state["x"] * 6364136223846793005 + 1) & 0xFFFFFFFFFFFFFFFF
                return state["x"]

            return next_u64

        a = Sampler([1] * 10, L=3, R=1000, seed=1, rng=make_rng())
        b = Sampler([1] * 10, L=3, R=1000, seed=1, rng=make_rng())
        self.assertEqual([a.sample() for _ in range(100)],
                         [b.sample() for _ in range(100)])
        a.close(); b.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
