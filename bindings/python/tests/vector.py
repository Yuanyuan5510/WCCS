"""Emit the fixed cross-language vector. Consumed by run_cross_lang.py.
SPDX-License-Identifier: Apache-2.0"""
from wccs import Sampler

with Sampler([1] * 10, L=3, beta_q=0, alpha_q=0, R=1000, seed=42, stream_id=0) as s:
    indices = [s.sample() for _ in range(100)]
print("indices:", *indices)
