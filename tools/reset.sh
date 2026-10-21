#!/usr/bin/env bash
# Сброс платы через USB-JTAG.
#
# Зачем: esptool сбрасывает чип через RTS, а на Cardputer RTS к выводу сброса не
# подключён. Единственный сброс, который здесь работает, - через USB-JTAG. Без
# него плата, попавшая в загрузчик (например, после ручного G0), там и остаётся:
# заливка «проходит» с проверкой хеша, а прошивка не стартует, и в логе висит
# "waiting for download". Проверено на живой плате - docs/board.md §12.
#
#   ./tools/reset.sh
#   ./tools/reset.sh /dev/cu.usbmodem101
#
# Код возврата: 0 - сброс прошёл, 1 - нет esptool или чип не ответил.

set -uo pipefail

cd "$(dirname "$0")/.."

PORT="${1:-}"

ESPTOOL="$HOME/.platformio/packages/tool-esptool/esptool"
if [ ! -x "$ESPTOOL" ]; then
    ESPTOOL="$(command -v esptool || true)"
fi
if [ -z "$ESPTOOL" ]; then
    echo "не найден esptool: ни в ~/.platformio/packages, ни в PATH" >&2
    exit 1
fi

# read-mac выбран потому, что сброс делает сам usb-reset, а read-mac ничего не
# пишет во flash: команда нужна только как повод поговорить с чипом.
if [ -n "$PORT" ]; then
    CMD=("$ESPTOOL" --chip esp32s3 -p "$PORT" --before usb-reset --after no-reset read-mac)
else
    CMD=("$ESPTOOL" --chip esp32s3 --before usb-reset --after no-reset read-mac)
fi

if ! output=$("${CMD[@]}" 2>&1); then
    echo "$output" >&2
    echo "сброс не удался: проверьте кабель и что порт не занят" >&2
    exit 1
fi

echo "плата сброшена"
