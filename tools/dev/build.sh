#!/bin/sh
# Build the on-desk test firmware into build-devtest/.
# Same sources and sdkconfig as the release build, plus sdkconfig.devtest
# (dev link on, touch-controller auto-upgrade off). The committed sdkconfig is
# not touched: the test build keeps its own copy inside the build directory.
set -eu
cd "$(dirname "$0")/../.."
: "${IDF_PATH:?run '. ~/esp/valet-idf-v6.1/export.sh' first}"
mkdir -p build-devtest
cat sdkconfig sdkconfig.devtest > build-devtest/sdkconfig.merged
# Keep the existing copy when nothing changed, so an incremental build stays incremental.
if ! cmp -s build-devtest/sdkconfig.merged build-devtest/sdkconfig.input 2>/dev/null; then
    cp build-devtest/sdkconfig.merged build-devtest/sdkconfig.input
    cp build-devtest/sdkconfig.merged build-devtest/sdkconfig
fi
exec idf.py -B build-devtest -D SDKCONFIG="$PWD/build-devtest/sdkconfig" build
