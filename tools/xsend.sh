#!/usr/bin/env bash
# Обёртка: берёт питон из PlatformIO (в нём есть pyserial).
#   ./tools/xsend.sh notes.txt /etc/notes.txt
#   ./tools/xsend.sh big.bin /big.bin /dev/cu.usbmodem1101
set -euo pipefail

cd "$(dirname "$0")/.."

PY=""
for c in /opt/homebrew/Cellar/platformio/*/libexec/bin/python; do
    [ -x "$c" ] && PY="$c"
done
if [ -z "$PY" ]; then
    PY=$(command -v python3)
    if ! "$PY" -c "import serial" 2>/dev/null; then
        echo "нет pyserial. Установите: python3 -m pip install pyserial" >&2
        exit 1
    fi
fi

exec "$PY" tools/xsend.py "$@"
