#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/valet-book-search.XXXXXX")
trap 'rm -rf "$work"' EXIT
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I "$root/main" \
  "$root/main/valet_book_search.cc" "$root/tools/tests/valet_book_search_test.cc" -o "$work/test"
"$work/test"
