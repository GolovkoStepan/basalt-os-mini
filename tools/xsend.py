#!/usr/bin/env python3
"""Положить файл на плату по XMODEM через ту же консоль, что и шелл.

    ./tools/xsend.sh firmware.bin /images/firmware.bin
    ./tools/xsend.sh notes.txt /etc/notes.txt --port /dev/cu.usbmodem1101

Почему XMODEM, а не scp/сеть: сети на плате нет, SD нет, а USB Serial/JTAG -
единственный канал, который под рукой. XMODEM переживает потерю байтов:
каждый пакет подтверждается, битый приходит заново.

Как это работает:

1. Скрипт открывает порт. Открытие перезапускает плату (так устроен USB-JTAG),
   поэтому он ждёт в логе `ready`.
2. Сам набирает команду `rz <путь>`: порт занят скриптом, набрать её руками в
   это время негде.
3. Ждёт метку `[rz]` - значит, эхо команды кончилось и плата слушает протокол.
4. Шлёт пакеты по 1 КБ (STX), с CRC-16 и подтверждением на каждый пакет.
5. После EOT печатает размер, время и скорость.

Ограничение, о котором надо помнить: XMODEM не передаёт длину файла. Последний
пакет дополняется байтом 0x1A, и приёмник выбрасывает этот хвост. Если файл
настоящий кончается на 0x1A, эти байты при заливке потеряются - так устроен
протокол, а не плата.
"""

import argparse
import sys
import time

import serial
from serial.tools import list_ports

# VID-ы плат: нативный USB Serial/JTAG (S3, S2, C3), мосты USB-UART.
KNOWN_VIDS = {0x303A, 0x1A86, 0x10C4, 0x0403}

SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
CRC_REQ = ord("C")
PAD = 0x1A

BLOCK = 1024       # 1 КБ на пакет: накладные расходы 0,5 % вместо 4 %
RETRIES = 10       # столько раз повторяем пакет, прежде чем сдаться
BLOCK_TIMEOUT = 3.0  # плата пишет пакет во flash, 3 с - с запасом


def find_port(explicit):
    if explicit:
        return explicit
    found = [p for p in list_ports.comports() if p.vid in KNOWN_VIDS]
    return found[0].device if found else None


def crc16(data):
    """CRC-16/XMODEM: полином 0x1021, начальное значение 0."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def open_board(port, deadline):
    """Открыть порт и дождаться `ready`.

    Переоткрываем порт в цикле: при перезапуске чипа USB-устройство на секунду
    исчезает, и уже открытый дескриптор перестаёт работать. Это не ошибка, а
    штатное поведение USB Serial/JTAG.
    """
    buf = bytearray()
    while time.time() < deadline:
        s = None
        try:
            s = serial.Serial(port, 115200, timeout=0.2, dsrdtr=False, rtscts=False)
            # DTR=0, RTS=1 на S3 - это команда «войти в загрузчик». Опускаем оба,
            # но не дёргаем: нам нужен сброс от открытия порта, а не download mode.
            s.dtr = False
            s.rts = False
            s.reset_input_buffer()

            while time.time() < deadline:
                chunk = s.read(4096)
                if not chunk:
                    continue
                buf += chunk
                if b"ready" in buf:
                    return s
        except (OSError, serial.SerialException):
            pass
        finally:
            if s is not None and b"ready" not in buf:
                try:
                    s.close()
                except Exception:
                    pass
        time.sleep(0.2)
    return None


def read_byte(s, timeout):
    old = s.timeout
    s.timeout = timeout
    try:
        return s.read(1)
    finally:
        s.timeout = old


def wait_marker(s, marker, timeout):
    """Дождаться метки, попутно печатая всё, что говорит плата."""
    deadline = time.time() + timeout
    seen = bytearray()
    while time.time() < deadline:
        chunk = s.read(256)
        if not chunk:
            continue
        seen += chunk
        if marker in seen:
            return True
    sys.stderr.write(seen.decode("utf-8", "replace"))
    return False


def wait_start(s, timeout=8.0):
    """Дождаться 'C' - приёмник просит начать передачу с CRC."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = read_byte(s, 0.2)
        if not b:
            continue
        if b[0] in (CRC_REQ, NAK):
            return b[0]
        if b[0] == CAN:
            return None
    return None


def wait_response(s, timeout=BLOCK_TIMEOUT):
    """Дождаться ACK/NAK/CAN, пропуская текст (эхо, отчёты платы)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = read_byte(s, 0.2)
        if not b:
            continue
        if b[0] in (ACK, NAK, CAN):
            return b[0]
    return None


def send_block(s, seq, data):
    """Отправить один пакет. True - подтверждён, False - отправитель сдался."""
    packet = bytearray()
    packet.append(STX)
    packet.append(seq & 0xFF)
    packet.append((~seq) & 0xFF)
    packet += data
    crc = crc16(data)
    packet.append(crc >> 8)
    packet.append(crc & 0xFF)

    for _ in range(RETRIES):
        s.write(packet)
        resp = wait_response(s)
        if resp == ACK:
            return True
        if resp == CAN:
            print("\nплата отменила передачу (CAN)", file=sys.stderr)
            return False
        # NAK или молчание - пакет пришёл битым или не дошёл, повторяем.
    return False


def send_file(s, data):
    total = len(data)
    blocks = max(1, (total + BLOCK - 1) // BLOCK)
    last_report = 0.0

    for index in range(blocks):
        chunk = data[index * BLOCK:(index + 1) * BLOCK]
        if len(chunk) < BLOCK:
            chunk = chunk + bytes([PAD]) * (BLOCK - len(chunk))

        if not send_block(s, index + 1, chunk):
            print(f"\nпакет {index + 1} из {blocks} не подтверждён", file=sys.stderr)
            return False

        now = time.time()
        if now - last_report >= 0.3 or index + 1 == blocks:
            last_report = now
            percent = 100 * (index + 1) // blocks
            print(f"\rотправлено {percent:3d}% ({index + 1}/{blocks} пакетов)", end="")
            sys.stdout.flush()

    print()

    # Конец файла. Приёмник подтверждает EOT отдельно; если ACK потерялся,
    # повторяем - иначе плата останется ждать до своего таймаута. ACK приходит
    # только после записи файла во flash, поэтому после него можно сразу
    # проверять файл на плате.
    for _ in range(RETRIES):
        s.write(bytes([EOT]))
        resp = wait_response(s, 2.0)
        if resp == ACK:
            return True
        if resp == CAN:
            print("\nплата отменила передачу на записи файла (CAN)", file=sys.stderr)
            return False
    print("EOT не подтверждён", file=sys.stderr)
    return False


def main():
    parser = argparse.ArgumentParser(
        description="Залить файл на плату по XMODEM через консоль USB Serial/JTAG."
    )
    parser.add_argument("file", help="файл на маке")
    parser.add_argument("remote", help="путь на плате, например /etc/notes.txt")
    parser.add_argument("-p", "--port", help="порт, по умолчанию ищется сам")
    parser.add_argument(
        "--boot-timeout", type=float, default=20.0,
        help="сколько ждать ready после открытия порта (по умолчанию 20 с)",
    )
    args = parser.parse_args()

    try:
        with open(args.file, "rb") as f:
            data = f.read()
    except OSError as exc:
        sys.exit(f"не прочитать {args.file}: {exc}")

    if len(data) == 0:
        print("файл пуст: плата создаст пустой файл", file=sys.stderr)

    # Путь уходит в командную строку шелла. Кавычки нужны для пробелов, а
    # кавычка внутри пути сломала бы разбор - такой путь не поддерживаем.
    if '"' in args.remote or "\n" in args.remote:
        sys.exit("путь на плате не должен содержать кавычек и переводов строк")
    if len(args.remote.encode()) >= 128:
        sys.exit("путь на плате длиннее 127 байт: VFS_PATH_MAX в прошивке")

    port = find_port(args.port)
    if port is None:
        sys.exit("порт не найден, проверьте: pio device list")

    print(f"# порт {port}, файл {args.file} ({len(data)} Б), на плате {args.remote}",
          file=sys.stderr)

    s = open_board(port, time.time() + args.boot_timeout)
    if s is None:
        sys.exit("плата не сказала ready: проверьте кабель и что порт не занят")

    try:
        # ^C стирает недонабранную строку: иначе rz приклеится к её хвосту.
        s.write(b"\x03")
        time.sleep(0.3)
        s.reset_input_buffer()

        # Набираем команду сами: порт занят этим скриптом, руками её не набрать.
        s.write(f'rz "{args.remote}"\r'.encode())
        if not wait_marker(s, b"[rz]", 10.0):
            sys.exit("плата не начала приём: нет метки [rz]")

        print("# плата готова, шлём XMODEM", file=sys.stderr)
        if wait_start(s) is None:
            sys.exit("плата не прислала 'C': приём не начался")

        started = time.time()
        ok = send_file(s, data)
        elapsed = time.time() - started

        if not ok:
            sys.exit(1)

        rate = len(data) / elapsed / 1024 if elapsed > 0 else 0
        print(f"# принято {len(data)} Б за {elapsed:.2f} с ({rate:.1f} КБ/с)",
              file=sys.stderr)
        print(f"# проверьте на плате: ls и cat {args.remote}", file=sys.stderr)
    finally:
        s.close()


if __name__ == "__main__":
    main()
