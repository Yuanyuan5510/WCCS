/* Build script: compile the C core as a static library from source.
 * This keeps the Rust crate self-contained and avoids any ABI/CRT mismatch
 * between a prebuilt MinGW library and the MSVC Rust toolchain.
 * SPDX-License-Identifier: Apache-2.0 */
use std::path::Path;

fn main() {
    // A published crate cannot reach outside its own directory, so the release
    // build vendors src/wccs.c + include/wccs.h into vendor/. When present we
    // use that copy; a source checkout uses the repository paths directly.
    let vendored = Path::new("vendor/wccs.c").exists();
    let (c_file, include_dir) = if vendored {
        ("vendor/wccs.c", "vendor")
    } else {
        ("../../src/wccs.c", "../../include")
    };

    let mut build = cc::Build::new();
    build
        .file(c_file)
        .include(include_dir)
        .define("WCCS_STATIC", None)
        .warnings(true);
    // MSVC's C compiler defaults to an older C mode; ask for C11 when available.
    build.flag_if_supported("/std:c11");
    build.compile("wccs");

    println!("cargo:rerun-if-changed={c_file}");
}
