#!/usr/bin/env bash
# Build the WASM fallback of the C core for Deno / Edge Workers / browsers.
# Requires Emscripten (emcc). Run from bindings/node/wasm/.
#
# The C core only uses <stdlib.h>/<string.h>/<stdint.h>, so it compiles
# unchanged to wasm32. WASM_BIGINT=1 maps the uint64/i64 parameters to JS
# BigInt, which wasm/loader.mjs uses to call the exports directly.
set -euo pipefail

cd "$(dirname "$0")"

emcc -O3 -std=c99 \
  -I ../../include \
  -s WASM=1 \
  -s WASM_BIGINT=1 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createWccsModule \
  -s ENVIRONMENT=web,worker,node \
  -s EXPORTED_FUNCTIONS='["_malloc","_free","_wccs_new_default","_wccs_sample","_wccs_free","_wccs_abi_version","_wccs_step","_wccs_total","_wccs_error_string"]' \
  -o wccs.mjs \
  ../../src/wccs.c

echo "Built wasm/wccs.mjs + wasm/wccs.wasm"
