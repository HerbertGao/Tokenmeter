#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
source "$ROOT/scripts/common.sh"
cd "$ROOT"

# These existing CLI modes need only the standard library, not Bluetooth packages.
for arg in "$@"; do
    if [[ "$arg" == --once || "$arg" == --help || "$arg" == -h ]]; then
        find_python
        exec "$PYTHON" -m daemon.tokenmeter "$@"
    fi
done

prepare_venv
"$PYTHON" -m pip --isolated install --disable-pip-version-check --no-user -r "$ROOT/daemon/requirements.txt"
printf '前台启动 Tokenmeter，默认每分钟推送；按 Ctrl+C 停止。请先停止另一台电脑上的 daemon。\n'
exec "$PYTHON" -m daemon.tokenmeter "$@"
