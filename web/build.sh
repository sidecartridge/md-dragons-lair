#!/usr/bin/env bash
#
# Builds the converter for a browser and for Node, with Emscripten in Docker
# (web/dist/dlconv.mjs and web/dist/dlconv.wasm), and the page that runs it
# (web/dist/site: serve it with any static server).
#
#   web/build.sh          the converter
#   web/build.sh test     also the host test of the converter, run in Node:
#                         the same CRC-32 as the native build's
#
# EMSDK_IMAGE overrides the pinned Emscripten image.

set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMSDK_IMAGE="${EMSDK_IMAGE:-emscripten/emsdk:6.0.11}"

run() {
  docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src \
    "$EMSDK_IMAGE" "$@"
}

CFLAGS="-std=c11 -O3 -Wall -Wextra -Werror -Wno-unknown-pragmas"
SOURCES="rp/src/convjob.c rp/src/convert.c rp/src/clip.c rp/src/mpeg1_video.c \
  rp/src/mpeg_ps.c rp/src/mp2_audio.c rp/src/picture16.c rp/src/cadence.c \
  rp/src/crc32.c rp/src/iso9660.c rp/src/manifest.c rp/src/game_table.c web/src/dlweb.c"

mkdir -p "$ROOT/web/dist"
# shellcheck disable=SC2086
run emcc $CFLAGS -I web/src -I rp/src/include $SOURCES \
  -sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web,worker,node \
  -sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8 -sEXPORTED_FUNCTIONS=_malloc,_free \
  -o web/dist/dlconv.mjs
echo "Built web/dist/dlconv.mjs and dlconv.wasm"

# The page: web/site's files and the converter, in web/dist/site.
rm -rf "$ROOT/web/dist/site"
mkdir -p "$ROOT/web/dist/site"
cp "$ROOT"/web/site/* "$ROOT/web/dist/dlconv.mjs" "$ROOT/web/dist/dlconv.wasm" \
  "$ROOT/web/dist/site/"
echo "Built the page in web/dist/site"

if [ "${1:-}" = "test" ]; then
  # The units test_convert.c names on its first line, as the host Makefile
  # builds them, with the host tests' stand-ins.
  UNITS="$(head -1 "$ROOT/tests/host/test_convert.c" | sed 's|^// units: ||')"
  mkdir -p "$ROOT/tests/host/build/web"
  # shellcheck disable=SC2086
  run emcc -std=c11 -O1 -Wall -Wextra -Werror -Wno-unknown-pragmas \
    -I tests/host/shim -I rp/src/include tests/host/test_convert.c $UNITS \
    -sNODERAWFS -sSTACK_SIZE=1048576 -sALLOW_MEMORY_GROWTH \
    -o tests/host/build/web/test_convert.js
  docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src/tests/host \
    "$EMSDK_IMAGE" node build/web/test_convert.js
fi
