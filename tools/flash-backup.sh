#!/usr/bin/env bash
# Бэкап / восстановление оригинальной прошивки CYD.
#
#   ./tools/flash-backup.sh backup            сохранить в backup/cyd_backup.bin
#   ./tools/flash-backup.sh restore           залить обратно
#   ./tools/flash-backup.sh flash-id          посмотреть чип и размер flash
#   ./tools/flash-backup.sh backup /dev/cu.usbserial-1234    порт вручную
#
set -euo pipefail

cd "$(dirname "$0")/.."

ACTION="${1:-backup}"
PORT="${2:-}"

ESPTOOL="$HOME/.platformio/packages/tool-esptool/esptool"
if [ ! -x "$ESPTOOL" ]; then
    ESPTOOL="$(command -v esptool || true)"
fi
if [ -z "$ESPTOOL" ]; then
    echo "esptool не найден. Выполните: pio pkg install -g -p platformio/tool-esptool"
    exit 1
fi

if [ -z "$PORT" ]; then
    PORT=$(ls /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* 2>/dev/null | head -1 || true)
fi
if [ -z "$PORT" ]; then
    echo "Порт не найден. Подключите плату micro-USB кабелем и проверьте: pio device list"
    exit 1
fi

echo "порт:    $PORT"
echo "esptool: $ESPTOOL"

mkdir -p backup

case "$ACTION" in
    backup)
        OUT="backup/cyd_backup.bin"
        if [ -f "$OUT" ]; then
            echo "$OUT уже существует - не перезаписываю"
            exit 0
        fi
        # на чтении CH340 не держит 460800 (Corrupt data) - читаем на 115200
        "$ESPTOOL" -p "$PORT" -b 115200 read-flash 0 0x400000 "$OUT"
        echo "готово: $OUT"
        ;;
    restore)
        IN="backup/cyd_backup.bin"
        if [ ! -f "$IN" ]; then
            echo "нет файла $IN"
            exit 1
        fi
        "$ESPTOOL" -p "$PORT" -b 460800 write-flash 0x0 "$IN"
        echo "готово: прошивка восстановлена"
        ;;
    flash-id)
        "$ESPTOOL" -p "$PORT" flash-id
        ;;
    *)
        echo "неизвестное действие: $ACTION (backup | restore | flash-id)"
        exit 1
        ;;
esac
