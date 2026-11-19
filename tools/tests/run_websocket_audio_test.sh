#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/valet-websocket.XXXXXX")
trap 'rm -rf "$work"' EXIT
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/main" -I /opt/homebrew/include/cjson \
  "$root/tools/tests/websocket_audio_test.cc" -o "$work/test"
"$work/test"
