#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
source "$ROOT/scripts/common.sh"

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    printf '用法：./flash.sh [串口]\n仅支持 Waveshare ESP32-S3-Touch-AMOLED-2.16。首次安装构建工具，选设备并输入 FLASH 后才构建/上传。\n'
    exit 0
fi
[[ -t 0 ]] || fail '刷机必须在交互终端确认；请从终端运行 ./flash.sh。'
prepare_venv
"$PYTHON" -m pip --isolated install --disable-pip-version-check --no-user -r "$ROOT/scripts/requirements-flash.txt"
exec "$PYTHON" "$ROOT/scripts/flash.py" "$@"
