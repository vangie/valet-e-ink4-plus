#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/valet-reading.XXXXXX")
trap 'rm -rf "$work"' EXIT
clang -c "$root/main/valet_crypto/monocypher.c" -o "$work/mono.o"
clang -c "$root/main/valet_crypto/monocypher-ed25519.c" -o "$work/ed.o"
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/main" -I /opt/homebrew/include/cjson \
  "$root/main/valet_reading_context.cc" "$root/tools/tests/valet_reading_context_test.cc" \
  "$work/mono.o" "$work/ed.o" -L /opt/homebrew/lib -lcjson -o "$work/test"
"$work/test"
