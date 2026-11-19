#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/valet-profiles.XXXXXX")
trap 'rm -rf "$work"' EXIT
for test in assistant_profiles assistant_profiles_storage; do
  sources="$root/main/assistant_profiles_model.cc"
  if [ "$test" = assistant_profiles_storage ]; then sources="$sources $root/main/assistant_profiles.cc"; fi
  clang++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I "$root/main" -I "$root/tools/tests/stubs" -I /opt/homebrew/include/cjson \
    $sources "$root/tools/tests/${test}_test.cc" -L /opt/homebrew/lib -lcjson -o "$work/$test"
  "$work/$test"
done
