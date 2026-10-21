#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1792540800}
MODE=${1:-package}
compile() {
    TARGET=$1
    mkdir -p "build/$TARGET"
    if [ "$TARGET" = native ]; then
        COMPILER=${CXX:-g++}
        EXTRA="-O2 -g"
    else
        SDK=${WEBOS_SDK:-$ROOT/.tools/sdk/arm-webos-linux-gnueabi_sdk-buildroot}
        if [ ! -x "$SDK/bin/arm-webos-linux-gnueabi-g++" ]; then
            python3 scripts/fetch-tools.py
        fi
        COMPILER=$SDK/bin/arm-webos-linux-gnueabi-g++
        EXTRA="-Os -static-libstdc++ -static-libgcc"
    fi
    # No shell-supplied/user configuration is interpreted by the runtime daemon.
    "$COMPILER" -std=c++11 $EXTRA -Wall -Wextra -Werror -Wpedantic -Wdate-time \
        -pthread -ffile-prefix-map="$ROOT"=. -fmacro-prefix-map="$ROOT"=. \
        -Idaemon -Ithird_party daemon/main.cpp daemon/common.cpp daemon/engine.cpp \
        daemon/backends/alsa.cpp daemon/network/rtp.cpp daemon/network/http.cpp \
        daemon/network/stream.cpp -ldl ${CXXFLAGS:-} -o "build/$TARGET/audiocastd"
    if [ "$TARGET" = native ]; then
        build/native/audiocastd --self-test
    else
        "$SDK/bin/arm-webos-linux-gnueabi-strip" build/webos/audiocastd
        "$SDK/bin/arm-webos-linux-gnueabi-readelf" -h build/webos/audiocastd | grep -E 'Class:|Machine:'
    fi
}
case "$MODE" in
    native) compile native ;;
    webos) compile webos ;;
    test)
        compile native
        python3 -m unittest discover -s tests -p 'test_*.py' -v
        find app service -name '*.js' -type f -exec node --check '{}' ';'
        ;;
    package)
        compile webos
        python3 scripts/package.py
        ;;
    *) echo "Usage: $0 [native|webos|test|package]" >&2; exit 2 ;;
esac
