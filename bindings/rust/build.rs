/* Build script: compile the C core as a static library from source.
 * This keeps the Rust crate self-contained and avoids any ABI/CRT mismatch
 * between a prebuilt MinGW library and the MSVC Rust toolchain.
 * SPDX-License-Identifier: Apache-2.0 */
fn main() {
    let mut build = cc::Build::new();
    build
        .file("../../src/wccs.c")
        .include("../../include")
        .define("WCCS_STATIC", None)
        .warnings(true);
    // MSVC's C compiler defaults to an older C mode; ask for C11 when available.
    build.flag_if_supported("/std:c11");
    build.compile("wccs");

    println!("cargo:rerun-if-changed=../../src/wccs.c");
    println!("cargo:rerun-if-changed=../../include/wccs.h");
}
