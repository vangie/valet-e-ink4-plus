#!/bin/sh
# macOS host check of the proxy client (HTTP CONNECT and SOCKS5) against
# small proxies and a target that the test starts on this machine.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/valet-net-proxy.XXXXXX")
trap 'rm -rf "$work"' EXIT
clang++ -std=c++17 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
  -I "$root/components/esp-ml307/include" -I "$root/tools/tests/stubs" \
  "$root/components/esp-ml307/src/esp/net_proxy.cc" "$root/tools/tests/net_proxy_test.cc" -o "$work/test"
"$work/test"
