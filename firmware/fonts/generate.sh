#!/bin/sh
set -eu
cd "$(dirname "$0")"
for size in 16 20 24 32 48 96; do
    range='0x20-0x7e,0xb7'
    if [ "$size" = 96 ]; then range='0x20,0x24,0x2c-0x2e,0x30-0x39,0x3c'; fi
    npm exec --yes --package=lv_font_conv@1.5.3 -- lv_font_conv \
        --font DejaVuSansMono.ttf --size "$size" --bpp 4 \
        --range "$range" --format lvgl --no-compress --no-prefilter \
        --lv-font-name "font_$size" --output "../src/fonts/font_$size.c"
done
