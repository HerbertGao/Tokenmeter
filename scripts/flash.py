"""Interactive uploader for the single supported board; no unattended confirmation bypass."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port', nargs='?', help='optional exact serial port from PlatformIO device list')
    args = parser.parse_args(argv)
    if not sys.stdin.isatty():
        print('刷机需要交互终端确认，不支持管道或无人值守上传。', file=sys.stderr)
        return 2
    try:
        result = subprocess.run(
            [sys.executable, '-m', 'platformio', 'device', 'list', '--json-output'],
            check=True, capture_output=True, text=True,
        )
        devices = json.loads(result.stdout)
        if not isinstance(devices, list):
            raise ValueError('PlatformIO 未返回串口列表')
        ports = [p for p in devices if isinstance(p, dict) and isinstance(p.get('port'), str)]
        if args.port is not None:
            candidates = [p for p in ports if p['port'] == args.port]
        else:
            candidates = [p for p in ports if isinstance(p.get('hwid'), str) and 'VID:PID=303A:1001' in p['hwid'].upper()]
        if not candidates:
            raise ValueError('未找到目标串口。检查 USB 数据线；必要时运行 pio device list，使用准确串口作为参数。')
        for index, device in enumerate(candidates, 1):
            print(f"{index}. {device['port']} | {device.get('description', '')} | {device.get('hwid', '')}")
        choice = 0
        if len(candidates) > 1:
            value = input('有多台设备，请输入目标编号（不会默认选择第一台）：').strip()
            if not value.isdecimal() or not 1 <= int(value) <= len(candidates):
                raise ValueError('编号无效，已取消刷机')
            choice = int(value) - 1
        port = candidates[choice]['port']
        print(f'目标串口：{port}\n仅支持 Waveshare ESP32-S3-Touch-AMOLED-2.16（16MB Flash / 8MB PSRAM）。')
        print('USB 标识不能证明板型，请核对实物。上传会覆盖现有固件；脚本不备份、不整片擦除。')
        if input('确认板型和串口无误，输入 FLASH 继续，其他输入取消：').strip() != 'FLASH':
            print('已取消，未构建或上传固件。')
            return 1
        subprocess.run([
            sys.executable, '-m', 'platformio', 'run', '-d', str(ROOT / 'firmware'),
            '-e', 'waveshare_amoled_216', '-t', 'upload', '--upload-port', port,
        ], check=True)
        print('构建和上传完成。换电脑连接已有固件时，不需要重新刷机。')
        return 0
    except (EOFError, KeyboardInterrupt):
        print('\n已取消。', file=sys.stderr)
        return 130
    except subprocess.CalledProcessError as error:
        print(f'PlatformIO 失败（退出码 {error.returncode}）。未报告成功，请检查上方错误或设备列表错误输出。', file=sys.stderr)
        if error.stdout:
            print(error.stdout, file=sys.stderr)
        if error.stderr:
            print(error.stderr, file=sys.stderr)
        return error.returncode if error.returncode > 0 else 1
    except (ValueError, OSError) as error:
        print(f'错误：{error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
