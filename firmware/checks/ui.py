#!/usr/bin/env python3
"""Native LVGL regression (no hardware). After the first `pio run` downloads libs:
python3 firmware/checks/ui.py [--render-dir docs/images]
Only --render-dir writes a project artifact: yesterday.png, an offline synthetic frame.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

FW = Path(__file__).resolve().parents[1]
LIBS = FW / '.pio/libdeps/waveshare_amoled_216'
LVGL = LIBS / 'lvgl'


def run(args, build):
    result = subprocess.run(list(map(str, args)), cwd=build, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(' '.join(map(str, args)) + '\n' + result.stdout + result.stderr)
    return result.stdout


def fixture():
    zone = datetime.timezone(datetime.timedelta(hours=8))
    return {
        'v': 1, 'date': '2026-10-17',
        'epoch': int(datetime.datetime(2026, 10, 17, 12, tzinfo=zone).timestamp()),
        'day_end': int(datetime.datetime(2026, 10, 18, tzinfo=zone).timestamp()),
        'utc_offset': 480, 'issues': 0, 'status': 'ok',
        'totals': {'cost': 0.79, 'tokens': 3160000, 'input': 258800, 'output': 24200,
                   'cache_read': 2877000, 'cache_write': 0, 'records': 34, 'unpriced': 0},
        'models': [
            {'provider': p, 'model': m, 'cost': c, 'tokens': t, 'records': r, 'unpriced': 0}
            for p, m, c, t, r in [
                ('openai-codex', 'gpt-5.4', 0.35, 1540000, 14),
                ('qoder', 'qoder-coder', 0.24, 980000, 10),
                ('openrouter', 'gpt-5.4', 0.13, 430000, 6),
                ('google', 'gemini-2.5-pro', 0.07, 210000, 4),
            ]
        ],
        'yesterday': {
            'date': '2026-10-16', 'issues': 0, 'status': 'ok',
            'totals': {'cost': 1.24, 'tokens': 2400000, 'input': 180000, 'output': 20000,
                       'cache_read': 2200000, 'cache_write': 0, 'records': 27, 'unpriced': 0},
        },
    }


def png565(data):
    assert len(data) == 480 * 480 * 2
    rgb = bytearray()
    for (v,) in struct.iter_unpack('<H', data):
        r, g, b = v >> 11, (v >> 5) & 63, v & 31
        rgb.extend(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    assert 5000 < sum(any(rgb[i:i+3]) for i in range(0, len(rgb), 3)) < 480 * 480 // 2

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    rows = b''.join(b'\0' + rgb[y*1440:(y+1)*1440] for y in range(480))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 480, 480, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--render-dir', type=Path, help='write only yesterday.png (offline, not a device photo)')
    args = parser.parse_args()
    sources = sorted((LVGL / 'src').rglob('*.c'))
    fonts = [FW / 'src/fonts' / f'font_{size}.c' for size in (16, 20, 24, 32, 48, 96)]
    if not sources or not all(font.is_file() for font in fonts) or not (LIBS / 'ArduinoJson/src/ArduinoJson.h').is_file():
        parser.error('Expected installed LVGL, the six UI fonts and ArduinoJson; run pio run in firmware first')
    with tempfile.TemporaryDirectory(prefix='tokenmeter-ui-') as temporary:
        build = Path(temporary)
        sample = build / 'sample.json'
        sample.write_text(json.dumps(fixture(), separators=(',', ':')))
        common = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                  '-fno-omit-frame-pointer',
                  '-I' + str(FW / 'include'), '-I' + str(LVGL), '-DLV_CONF_INCLUDE_SIMPLE',
                  '-DLV_LVGL_H_INCLUDE_SIMPLE', '-DLV_BUILD_EXAMPLES=0', '-DLV_BUILD_DEMOS=0']

        def compile_one(pair):
            i, source = pair
            obj = build / f'{i}.o'
            run(['cc', '-std=c99', *common, '-c', source, '-o', obj], build)
            return obj

        print(f'Compiling real LVGL: {len(sources)} C files + 6 fonts; 8 workers; ASan/UBSan on all TUs', flush=True)
        with ThreadPoolExecutor(max_workers=8) as pool:
            objects = list(pool.map(compile_one, enumerate(sources + fonts)))
        archive = build / 'liblvgl.a'
        run(['ar', 'rcs', archive, *objects], build)
        binary = build / 'ui'
        run(['c++', '-std=c++17', *common, '-I' + str(LIBS / 'ArduinoJson/src'),
             FW / 'checks/ui.cpp', FW / 'src/core.cpp', archive, '-o', binary], build)
        command = [binary, sample]
        if args.render_dir:
            command.append(build / 'yesterday.rgb565')
        print(run(command, build), end='', flush=True)
        if args.render_dir:
            encoded = png565((build / 'yesterday.rgb565').read_bytes())
            args.render_dir.mkdir(parents=True, exist_ok=True)
            output = args.render_dir / 'yesterday.png'
            output.write_bytes(encoded)
            print(f'{output}: 480x480 synthetic offline LVGL render; no text metadata')


if __name__ == '__main__':
    main()
