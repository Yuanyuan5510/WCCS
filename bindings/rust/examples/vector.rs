//! Cross-language vector emitter for the Rust binding.
//! SPDX-License-Identifier: Apache-2.0
use wccs::Sampler;

fn main() {
    let w = [1u64; 10];
    let mut s = Sampler::new(&w, 3, 0, 0, 1000, 42, 0).expect("init");
    let mut out: Vec<String> = Vec::with_capacity(100);
    for _ in 0..100 {
        out.push(s.sample().expect("sample").to_string());
    }
    println!("indices: {}", out.join(" "));
}
