#!/usr/bin/env bash
# Обёртка: берёт питон из PlatformIO (в нём есть pyserial).
#   ./tools/serial-read.sh 10
#   ./tools/serial-read.sh 10 /dev/cu.usbserial-1130
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

exec "$PY" tools/serial-read.py "$@"
