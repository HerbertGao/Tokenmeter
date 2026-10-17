#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .pio/host
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -DARDUINOJSON_USE_LONG_LONG=1 -DARDUINOJSON_USE_DOUBLE=1 \
    -Isrc -I.pio/libdeps/waveshare_amoled_216/ArduinoJson/src \
    src/core.cpp checks/host.cpp -o .pio/host/check
.pio/host/check "$@"
