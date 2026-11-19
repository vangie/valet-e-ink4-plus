#!/bin/sh
# macOS host check against the same Mbed TLS base64 implementation as ESP-IDF.
set -eu
: "${IDF_PATH:?Set IDF_PATH to ESP-IDF v6.1}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
crypto="$IDF_PATH/components/mbedtls/mbedtls/tf-psa-crypto"
work=$(mktemp -d "${TMPDIR:-/tmp}/valet-pairing.XXXXXX")
trap 'rm -rf "$work"' EXIT
printf '%s\n' '#define MBEDTLS_BASE64_C' > "$work/crypto_config.h"
for source in base64 constant_time; do
  clang -c "$crypto/utilities/$source.c" -DTF_PSA_CRYPTO_CONFIG_FILE='"crypto_config.h"' \
    -I "$work" -I "$crypto/include" -I "$crypto/drivers/builtin/include" \
    -I "$crypto/core" -I "$crypto/utilities" -o "$work/$source.o"
done
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DTF_PSA_CRYPTO_CONFIG_FILE='"crypto_config.h"' -I "$work" \
  -I "$crypto/include" -I "$crypto/drivers/builtin/include" \
  -I "$root/main" -I /opt/homebrew/include/cjson \
  "$root/main/assistant_profiles_model.cc" "$root/main/valet_setup/pairing.cc" \
  "$root/tools/tests/valet_pairing_test.cc" "$work/base64.o" "$work/constant_time.o" \
  -L /opt/homebrew/lib -lcjson -o "$work/test"
"$work/test"
