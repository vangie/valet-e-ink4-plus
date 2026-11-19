#!/bin/sh
set -eu
: "${IDF_PATH:?Set IDF_PATH to ESP-IDF v6.1}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
crypto="$IDF_PATH/components/mbedtls/mbedtls/tf-psa-crypto"
work=$(mktemp -d "${TMPDIR:-/tmp}/valet-wire.XXXXXX")
trap 'rm -rf "$work"' EXIT
printf '%s\n' '#define MBEDTLS_BASE64_C' > "$work/crypto_config.h"
for source in base64 constant_time; do
  clang -c "$crypto/utilities/$source.c" -DTF_PSA_CRYPTO_CONFIG_FILE='"crypto_config.h"' \
    -I "$work" -I "$crypto/include" -I "$crypto/drivers/builtin/include" \
    -I "$crypto/core" -I "$crypto/utilities" -o "$work/$source.o"
done
clang -c "$root/main/valet_crypto/monocypher.c" -o "$work/mono.o"
clang -c "$root/main/valet_crypto/monocypher-ed25519.c" -o "$work/ed.o"
clang++ -std=c++17 -Wall -Wextra -Werror -DTF_PSA_CRYPTO_CONFIG_FILE='"crypto_config.h"' \
  -I "$work" -I "$crypto/include" -I "$crypto/drivers/builtin/include" -I "$root/main" \
  -I /opt/homebrew/include/cjson "$root/main/protocols/valet_wire.cc" "$root/tools/tests/valet_wire_test.cc" \
  "$work/base64.o" "$work/constant_time.o" "$work/mono.o" "$work/ed.o" \
  -L /opt/homebrew/lib -lcjson -o "$work/wire"
VALET_WIRE_TEST="$work/wire" "${VALET_TEST_PYTHON:-python3}" "$root/tools/tests/test_valet_gateway.py"
