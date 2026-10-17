#!/bin/bash

fail() {
    printf '错误：%s\n' "$*" >&2
    exit 1
}

find_python() {
    local candidate
    for candidate in "$ROOT/.venv/bin/python" python3.14 python3.13 python3.12 python3.11 python3.10 /opt/homebrew/bin/python3 /usr/local/bin/python3 python3; do
        if command -v "$candidate" >/dev/null 2>&1 &&
           "$candidate" -c 'import sys; sys.exit(sys.version_info < (3, 10))' >/dev/null 2>&1; then
            PYTHON="$(command -v "$candidate")"
            return
        fi
    done
    fail '请先安装 Python 3.10+（推荐 3.14），然后重新运行。不会自动安装系统 Python。'
}

prepare_venv() {
    if [[ ! -e "$ROOT/.venv" && ! -L "$ROOT/.venv" ]]; then
        find_python
        "$PYTHON" -m venv "$ROOT/.venv"
    fi
    PYTHON="$ROOT/.venv/bin/python"
    [[ -x "$PYTHON" ]] && "$PYTHON" -c 'import sys; sys.exit(sys.version_info < (3, 10) or sys.prefix == sys.base_prefix)' >/dev/null 2>&1 ||
        fail '已有 .venv 不是可用的 Python 3.10+ 虚拟环境；请手动修复或改名后重试，脚本不会删除它。'
}
