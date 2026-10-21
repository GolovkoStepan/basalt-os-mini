#!/usr/bin/env python3
"""Прочитать лог с serial-порта заданное время.

    ./tools/serial-read.py              # 10 секунд, порт определяется сам
    ./tools/serial-read.py 5            # 5 секунд
    ./tools/serial-read.py 5 /dev/cu.usbmodem1101

Или просто: ./tools/serial-read.sh 10

Порт выбирается по USB-VID, а не по имени файла устройства. Имя зависит от
чипа: у мостов USB-UART это `usbserial-*`, у нативного USB Serial/JTAG
(ESP32-S3, S2, C3) — `usbmodem*`.

ВАЖНО, И ЭТО СТОИЛО ПОЛУДНЯ ОТЛАДКИ: DTR и RTS здесь НЕ ТРОГАЮТСЯ.
На USB Serial/JTAG (Cardputer, любой S3 без моста UART) комбинация
`DTR=0, RTS=1` — это не «сброс», а команда «войти в режим загрузчика».
Скрипт, который так делал, переводил чип в download mode и оставлял его там:
чип жив, порт есть, esptool отвечает, а прошивка не исполняется и лог пуст.
Выглядит как «плата сломалась».

Правильный сброс для S3 — через esptool, он умеет это по USB:
    esptool --chip esp32s3 -p <порт> --before no-reset --after hard-reset read-mac
После него порт пересоздаётся, поэтому читать надо с паузой.

pyserial не поднимает линии сам (dsrdtr=False, rtscts=False) — только слушаем.
"""
import sys
import time

import serial
from serial.tools import list_ports

# VID-ы, которые встречаются на платах ESP32:
#   0x303A Espressif — нативный USB Serial/JTAG (Cardputer, S3)
#   0x1A86 QinHeng  — CH340 / CH9102 (CYD)
#   0x10C4 Silicon Labs — CP210x
#   0x0403 FTDI
KNOWN_VIDS = {0x303A, 0x1A86, 0x10C4, 0x0403}

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
PORT = sys.argv[2] if len(sys.argv) > 2 else None

if PORT is None:
    found = [p for p in list_ports.comports() if p.vid in KNOWN_VIDS]
    if not found:
        sys.exit('порт не найден, проверьте: pio device list')
    PORT = found[0].device

print(f'# порт {PORT}, {DURATION:.0f} c', file=sys.stderr)

with serial.Serial(PORT, 115200, timeout=0.2, dsrdtr=False, rtscts=False) as s:
    # Явно опустить, но не переключать: RTS=1 на S3 означает режим загрузчика.
    s.dtr = False
    s.rts = False
    time.sleep(0.3)
    s.reset_input_buffer()

    end = time.time() + DURATION
    buf = b''
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            buf += chunk

if buf:
    sys.stdout.write(buf.decode('utf-8', 'replace'))
    print(f'\n# получено {len(buf)} Б', file=sys.stderr)
else:
    print('# ни одного байта. Если прошивка точно залита, чип, вероятно, остался\n'
          '# в режиме загрузчика: сбросьте его через esptool (см. шапку файла).',
          file=sys.stderr)
sys.stdout.flush()
