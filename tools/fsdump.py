#!/usr/bin/env python3
"""Разобрать ФС BFS по дампу раздела flash — без участия платы.

    ./tools/fsdump.py                      # снять дамп с платы и показать дерево
    ./tools/fsdump.py --save dump.bin      # то же, но дамп остаётся на маке
    ./tools/fsdump.py --read dump.bin      # разобрать готовый дамп, плата не нужна
    ./tools/fsdump.py -l /etc              # один каталог
    ./tools/fsdump.py -c /etc/rc           # содержимое файла в stdout
    ./tools/fsdump.py -o rc.txt /etc/rc    # выгрузить файл на мак
    ./tools/fsdump.py --super              # суперблок, inode и сверка блоков

Дерево печатается с меткой времени файла в том же виде, что `ls -l` на
плате (`07.11 18:42`), а `--super` добавляет счётчик inode с меткой: ноль в
поле mtime означает «время неизвестно» и печатается прочерком, а не 1970-м.

Зачем это нужно. ФС — своя (storage/bfs.c), и её формат лежит на диске байтами,
а не за чужим API. Значит, содержимое можно прочитать и тогда, когда прошивка
не работает: паника, зависший USB, плата не стартует. В этом случае остаётся
esptool над загрузчиком — он читает flash, не исполняя наш код, — а всё
остальное делает этот скрипт на маке.

Почему чтение одним куском, а не «умное» по блокам. esptool на этой плате
отдаёт около 175 КБ/с и тратит примерно 6 секунд на каждый запуск: сброс,
синхронизация, загрузка заглушки. Раздел storage — 14,6 МБ, то есть полторы
минуты одним вызовом против шести секунд накладных на каждый блок при
поштучном чтении. Один раз дольше — зато дамп согласован: пока идёт чтение,
прошивка не работает и ничего не меняет. Сохранённый дамп разбирается
мгновенно, поэтому `--save` полезен, когда в ФС придётся копаться.

Чего скрипт не делает: не пишет на плату. Даже снятый дамп он никуда не
заливает. Запись в ФС — через живую систему (`echo`, `tools/xsend.sh`), а не
через байты на диске: у BFS копирование при записи, и правка блока снаружи
рассинхронизировала бы суперблок с деревом.
"""

import argparse
import datetime
import os
import struct
import subprocess
import sys
import tempfile
import zlib

# ---------------------------------------------------------------------------
# Формат BFS (storage/bfs.h, storage/bfs.c)
#
# Числа повторены здесь, а не взяты из .h: скрипт запускается на маке, где нет
# ни препроцессора, ни xtensa-заголовков. Расхождение с прошивкой поймает
# `--super`: он сверяет CRC, размеры структур и счётчики.
# ---------------------------------------------------------------------------

BLOCK = 4096
NAME_MAX = 24
INODE_SIZE = 128
INODES_PER_BLOCK = BLOCK // INODE_SIZE
INODE_COUNT = 256
SUPER_SLOTS = 32
SUPER_SLOT = 128
SUPER_SECTORS = 2
DIRECT_BLOCKS = 12
PTRS_PER_BLOCK = BLOCK // 4

SUPER_MAGIC = 0x53465342  # «BSFS» в little-endian
SUPER_VERSION = 1

TYPE_FILE = 0x8000
TYPE_DIR = 0x4000
TYPE_MASK = 0xF000

DIRENT_SIZE = 32
DIRENTS_PER_BLOCK = BLOCK // DIRENT_SIZE

# Таблица разделов: формат IDF, записи по 32 байта с 0x8000.
PART_TABLE_ADDR = 0x8000
PART_TABLE_MAX = 0xC00
PART_MAGIC = 0x50AA
PART_MAGIC_MD5 = 0xEBEB

# Как искать плату: у нативного USB Serial/JTAG это usbmodem, у моста — usbserial.
# Имена на маке зависят от чипа и моста, поэтому список, а не одно имя.
PORT_PREFIXES = ("/dev/cu.usbmodem", "/dev/cu.usbserial")

# 6 секунд — запуск esptool, 175 КБ/с — замер на этой плате (docs/board.md §5).
ESPTOOL_STARTUP_S = 6.0
ESPTOOL_RATE = 175 * 1024

SUPER = struct.Struct("<IHHIIII8II")  # 60 байт из 128-байтного слота
INODE = struct.Struct("<IIII12III14I")  # ровно 128 байт
DIRENT = struct.Struct("<IHBB24s")  # ровно 32 байта


class FsError(Exception):
    """ФС не разобралась. Текст показывается человеку как есть."""


class Device:
    """Раздел flash как последовательность блоков."""

    def __init__(self, image, base):
        self.image = image
        self.base = base

    @property
    def blocks(self):
        return len(self.image) // BLOCK

    def block(self, number):
        offset = number * BLOCK
        if offset + BLOCK > len(self.image):
            raise FsError(
                f"блок {number} за пределами дампа ({self.blocks} блоков): "
                "дамп обрезан"
            )
        return self.image[offset:offset + BLOCK]


# ---------------------------------------------------------------------------
# Суперблок
# ---------------------------------------------------------------------------

def parse_super(dev):
    """Найти самый свежий целый суперблок.

    Кольцо из двух секторов по 32 слота: суперблок пишется по слотам подряд, а
    сектор меняется и стирается раз в 32 коммита. Поэтому валидных копий на
    диске много, и брать надо не первую, а с наибольшим поколением — иначе
    разбор показал бы состояние месячной давности.
    """
    best = None
    for sector in range(SUPER_SECTORS):
        if (sector + 1) * BLOCK > len(dev.image):
            break
        raw = dev.image[sector * BLOCK:(sector + 1) * BLOCK]
        for slot in range(SUPER_SLOTS):
            chunk = raw[slot * SUPER_SLOT:slot * SUPER_SLOT + SUPER.size]
            if len(chunk) < SUPER.size:
                break
            fields = SUPER.unpack(chunk)
            if fields[0] != SUPER_MAGIC or fields[1] != SUPER_VERSION:
                continue
            # Слот с неверным CRC — это оборванный коммит (или стираемый
            # сектор). Такая копия не считается и не мешает найти живую.
            if zlib.crc32(chunk[:-4]) & 0xFFFFFFFF != fields[-1]:
                continue
            candidate = {
                "generation": fields[3],
                "block_count": fields[4],
                "root_inode": fields[5],
                "inode_count": fields[6],
                "inode_table": list(fields[7:7 + INODE_COUNT // INODES_PER_BLOCK]),
                "sector": sector,
                "slot": slot,
            }
            if best is None or candidate["generation"] > best["generation"]:
                best = candidate

    if best is None:
        raise FsError(
            "суперблок BFS не найден: раздел не размечен или дамп снят не с "
            "того места"
        )
    if not 16 <= best["block_count"] <= 4096:
        raise FsError(f"в суперблоке негодное число блоков: {best['block_count']}")
    if not 1 <= best["root_inode"] < INODE_COUNT:
        raise FsError(f"в суперблоке негодный корневой inode: {best['root_inode']}")
    return best


class Fs:
    def __init__(self, dev):
        self.dev = dev
        self.super = parse_super(dev)

        # Дамп короче раздела — это не отказ: в ФС может не быть файлов за
        # границей обрезки. Но узнавать об этом из «блок N за пределами дампа»
        # поздно и непонятно, поэтому говорим сразу.
        if dev.blocks < self.super["block_count"]:
            print(
                f"# дамп короче раздела: {dev.blocks} блоков из "
                f"{self.super['block_count']}; файлы за границей не читаются",
                file=sys.stderr,
            )

    # -- inode ---------------------------------------------------------------

    def inode(self, index):
        if index >= INODE_COUNT:
            raise FsError(f"inode {index} за пределами таблицы ({INODE_COUNT})")
        block = self.super["inode_table"][index // INODES_PER_BLOCK]
        if block == 0:
            raise FsError(f"inode {index}: блок таблицы inode не задан")
        offset = (index % INODES_PER_BLOCK) * INODE_SIZE
        fields = INODE.unpack(self.dev.block(block)[offset:offset + INODE_SIZE])
        return {
            "index": index,
            "mode": fields[0],
            "size": fields[1],
            "mtime": fields[2],
            "links": fields[3],
            "direct": list(fields[4:4 + DIRECT_BLOCKS]),
            "indirect": fields[4 + DIRECT_BLOCKS],
            "double": fields[5 + DIRECT_BLOCKS],
        }

    def kind(self, ino):
        if ino["mode"] == 0:
            return "свободен"
        if (ino["mode"] & TYPE_MASK) == TYPE_DIR:
            return "каталог"
        if (ino["mode"] & TYPE_MASK) == TYPE_FILE:
            return "файл"
        return f"неизвестный тип {ino['mode']:#x}"

    # -- карта блоков --------------------------------------------------------

    def pointers(self, block):
        return struct.unpack(f"<{PTRS_PER_BLOCK}I", self.dev.block(block))

    def file_block(self, ino, index):
        """Номер блока с данными или 0 для дырки.

        Дырка — это не «ещё не записано», а нормальное состояние: размер файла
        и число занятых блоков у BFS связаны не жёстко (усечение освобождает
        блоки, а увеличение читает дырку нулями).
        """
        if index < DIRECT_BLOCKS:
            return ino["direct"][index]
        index -= DIRECT_BLOCKS

        if index < PTRS_PER_BLOCK:
            if ino["indirect"] == 0:
                return 0
            return self.pointers(ino["indirect"])[index]
        index -= PTRS_PER_BLOCK

        if ino["double"] == 0:
            return 0
        l1, l2 = divmod(index, PTRS_PER_BLOCK)
        mid = self.pointers(ino["double"])[l1]
        if mid == 0:
            return 0
        return self.pointers(mid)[l2]

    def file_blocks(self, ino):
        """Занятые блоки файла или каталога. Дырки пропускаются."""
        count = (ino["size"] + BLOCK - 1) // BLOCK
        return [b for b in (self.file_block(ino, i) for i in range(count)) if b != 0]

    def read_file(self, ino):
        chunks = []
        left = ino["size"]
        index = 0
        while left > 0:
            block = self.file_block(ino, index)
            data = self.dev.block(block) if block != 0 else bytes(BLOCK)
            chunks.append(data[:min(left, BLOCK)])
            left -= min(left, BLOCK)
            index += 1
        return b"".join(chunks)

    def metadata_blocks(self, ino):
        """Блоки косвенности: сами не данные, но место занимают."""
        out = []
        if ino["indirect"] != 0:
            out.append(ino["indirect"])
        if ino["double"] != 0:
            out.append(ino["double"])
            out.extend(b for b in self.pointers(ino["double"]) if b != 0)
        return out

    # -- каталоги ------------------------------------------------------------

    def entries(self, ino):
        """Живые записи каталога. Надгробия (inode 0) пропускаются.

        Размер каталога — длина занятой части записей, а не число файлов:
        удаление обнуляет inode, но блок не сжимает, иначе «удалить в
        середине» переписывало бы весь каталог.
        """
        out = []
        count = (ino["size"] + BLOCK - 1) // BLOCK
        for index in range(count):
            block = self.file_block(ino, index)
            if block == 0:
                continue
            raw = self.dev.block(block)
            for k in range(DIRENTS_PER_BLOCK):
                entry = raw[k * DIRENT_SIZE:(k + 1) * DIRENT_SIZE]
                # Пять полей: inode, тип, длина имени, резерв и само имя. Имя
                # берём срезом по длине, а не из поля: в name остаётся мусор от
                # прежней записи, если файл переименовали на более короткое имя.
                inode_no, etype, name_len, _, _ = DIRENT.unpack(entry)
                if inode_no == 0:
                    continue
                name = entry[8:8 + min(name_len, NAME_MAX)].decode("utf-8", "replace")
                out.append({"inode": inode_no, "type": etype, "name": name})
        return out

    def resolve(self, path):
        """Путь -> inode. Разбор как в VFS: пустые части и `.` пропускаются."""
        if not path.startswith("/"):
            raise FsError(f"путь должен быть абсолютным: {path}")
        current = self.inode(self.super["root_inode"])
        for part in path.split("/"):
            if part in ("", "."):
                continue
            if self.kind(current) != "каталог":
                raise FsError(f"{path}: не каталог на пути к «{part}»")
            found = next((e for e in self.entries(current) if e["name"] == part), None)
            if found is None:
                raise FsError(f"{path}: не найдено")
            current = self.inode(found["inode"])
        return current


# ---------------------------------------------------------------------------
# Обход дерева
# ---------------------------------------------------------------------------

def walk(fs, ino, path, items, seen, problems):
    for entry in fs.entries(ino):
        child_path = path.rstrip("/") + "/" + entry["name"]
        if entry["inode"] >= INODE_COUNT:
            problems.append(f"{child_path}: ссылка на inode {entry['inode']} вне таблицы")
            continue
        if entry["inode"] in seen:
            problems.append(f"{child_path}: inode {entry['inode']} уже встречался (цикл?)")
            continue
        child = fs.inode(entry["inode"])
        kind = fs.kind(child)
        if kind == "свободен":
            problems.append(f"{child_path}: ссылка на свободный inode {entry['inode']}")
            continue
        seen.add(entry["inode"])
        items.append({"path": child_path, "kind": kind,
                      "size": child["size"], "mtime": child["mtime"],
                      "inode": entry["inode"]})
        if kind == "каталог":
            walk(fs, child, child_path, items, seen, problems)


def collect(fs):
    root = fs.inode(fs.super["root_inode"])
    seen = {fs.super["root_inode"]}
    problems = []
    items = []
    walk(fs, root, "/", items, seen, problems)
    return root, items, seen, problems


# ---------------------------------------------------------------------------
# Вывод
# ---------------------------------------------------------------------------

# Метка времени в BFS — секунды Unix, 0 значит «время неизвестно». Формат тот
# же, что у `ls -l` на плате, включая прочерк: иначе разбирающий дамп и
# смотрящий на экран видели бы одно и то же поле в двух разных видах и спорили
# бы, где ошибка.
def time_text(seconds):
    if seconds == 0:
        return "--.-- --:--"
    try:
        moment = datetime.datetime.fromtimestamp(seconds, datetime.timezone.utc)
    except (OverflowError, OSError, ValueError):
        return f"?{seconds}"
    return moment.strftime("%d.%m %H:%M")

def kib(value):
    if value < 1024:
        return f"{value} Б"
    return f"{value / 1024:.1f} КБ"


def human_time(seconds):
    return f"{seconds:.0f} с" if seconds < 90 else f"{seconds / 60:.1f} мин"


def print_tree(items, root_path):
    root_path = root_path.rstrip("/")
    prefix = root_path + "/" if root_path else "/"
    subset = [i for i in items if i["path"].startswith(prefix)]
    if not subset:
        print("(пусто)")
        return

    for item in subset:
        relative = item["path"][len(prefix):]
        name = relative.rsplit("/", 1)[-1]
        indent = "  " * relative.count("/")
        if item["kind"] == "каталог":
            print(f"{time_text(item['mtime'])}  {indent}{name}/")
        else:
            print(f"{time_text(item['mtime'])}  {indent}{name}  {item['size']} Б")

    files = sum(1 for i in subset if i["kind"] == "файл")
    dirs = sum(1 for i in subset if i["kind"] == "каталог")
    print(f"\nвсего: {dirs} каталогов, {files} файлов")


def print_problems(problems):
    if not problems:
        return
    print("\nрасхождения:")
    for text in problems:
        print(f"  {text}")


def show_super(fs, seen, problems):
    sup = fs.super
    print(f"поколение {sup['generation']}, суперблок в секторе {sup['sector']}, "
          f"слот {sup['slot']}")
    print(f"блоков {sup['block_count']} по {BLOCK} Б = "
          f"{kib(sup['block_count'] * BLOCK)}")
    print(f"корневой inode {sup['root_inode']}, inode в таблице {sup['inode_count']}")
    print("таблица inode: блоки " + ", ".join(str(b) for b in sup["inode_table"]))

    # Битмап BFS на диске не хранит: он собирается обходом inode-таблицы при
    # монтировании. Поэтому сверять можно только то, что видно: блоки не
    # должны принадлежать двум inode сразу и не должны выходить за раздел.
    used = set(range(SUPER_SECTORS)) | set(sup["inode_table"])
    doubled = []
    outside = []
    live = 0
    for index in range(INODE_COUNT):
        ino = fs.inode(index)
        if ino["mode"] == 0:
            continue
        live += 1
        for block in fs.metadata_blocks(ino) + fs.file_blocks(ino):
            if block >= sup["block_count"]:
                outside.append((index, block))
                continue
            if block in used:
                doubled.append((index, block))
            used.add(block)

    print(f"\ninode занято {live} из {INODE_COUNT}")
    print(f"блоков занято {len(used)} из {sup['block_count']} "
          f"({len(used) * BLOCK / 1024:.1f} КБ)")

    # Метки времени: сколько файлов их имеет. Ноль в этом поле — не ошибка
    # (старый образ или запись без установленных часов), но знать, сколько
    # таких, полезно: иначе «у половины файлов прочерк» выясняется по одному.
    stamped = sum(1 for i in range(INODE_COUNT)
                  if fs.inode(i)["mode"] != 0 and fs.inode(i)["mtime"] != 0)
    print(f"inode с меткой времени: {stamped} из {live}")

    for index, block in doubled[:10]:
        problems.append(f"блок {block} занят дважды (inode {index})")
    for index, block in outside[:10]:
        problems.append(f"inode {index}: блок {block} за пределами раздела")

    orphans = [i for i in range(INODE_COUNT)
               if i not in seen and fs.inode(i)["mode"] != 0]
    if orphans:
        print(f"недостижимых из дерева inode: {len(orphans)} "
              f"(первый — {orphans[0]}): место занимают, а файла за ними нет")

    print()
    if problems:
        print_problems(problems)
    else:
        print("расхождений нет: дерево, inode и блоки сходятся")


# ---------------------------------------------------------------------------
# Чтение с платы
# ---------------------------------------------------------------------------

def find_port():
    for entry in sorted(os.listdir("/dev")):
        path = "/dev/" + entry
        if path.startswith(PORT_PREFIXES):
            return path
    return None


def find_esptool():
    candidate = os.path.expanduser("~/.platformio/packages/tool-esptool/esptool")
    if os.access(candidate, os.X_OK):
        return candidate
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        path = os.path.join(directory, "esptool")
        if os.access(path, os.X_OK):
            return path
    return None


def parse_partition_table(table):
    entries = []
    for index in range(len(table) // 32):
        entry = table[index * 32:(index + 1) * 32]
        magic = struct.unpack_from("<H", entry)[0]
        if magic == PART_MAGIC_MD5 or (entry[0] == 0xFF and entry[1] == 0xFF):
            break  # MD5 в конце таблицы или стёртая flash: записей больше нет
        if magic != PART_MAGIC:
            break
        address, size = struct.unpack_from("<II", entry, 4)
        label = entry[12:28].split(b"\0")[0].decode("utf-8", "replace")
        entries.append({"type": entry[2], "subtype": entry[3],
                        "address": address, "size": size, "label": label})
    return entries


def run_esptool(port, arguments):
    """Запустить esptool и вернуть его вывод. Отказ — FsError с хвостом лога."""
    esptool = find_esptool()
    if esptool is None:
        raise FsError("не найден esptool: нет ни в ~/.platformio/packages, ни в PATH")

    command = [esptool, "--chip", "esp32s3", "-p", port] + arguments
    # Вывод esptool — прогресс-бар на весь экран, в разборе он бесполезен.
    # Зато при отказе нужен его текст, поэтому ловим и то и другое.
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        tail = result.stdout.strip().splitlines()[-3:]
        raise FsError(
            "esptool не смог связаться с платой. Если порт есть, а чип не "
            "отвечает — переподключите USB-кабель: USB-JTAG иногда "
            "зависает (docs/board.md §5).\n  " + "\n  ".join(tail)
        )
    return result.stdout


def board_read(port, address, size):
    """Прочитать кусок flash. Плата остаётся в загрузчике: сброс — отдельно.

    `--after no-reset` здесь не осторожность, а единственный рабочий вариант.
    Любой сброс *из* загрузчика (`hard-reset` по RTS, `watchdog-reset`) после
    долгого чтения оставляет USB-JTAG мёртвым: порт есть, байтов нет, и
    помогает только передёргивание питания. Проверено 06.11.2026: короткое
    чтение с `watchdog-reset` плата переживала, полный дамп раздела (96 с) —
    нет. Поэтому выход из загрузчика делается один раз и по-другому, см.
    `board_wake` (docs/board.md §5).
    """
    fd, path = tempfile.mkstemp(prefix="fsdump-", suffix=".bin")
    os.close(fd)
    try:
        run_esptool(port, ["--before", "default-reset", "--after", "no-reset",
                           "read-flash", hex(address), hex(size), path])
        with open(path, "rb") as handle:
            image = handle.read()
        if len(image) != size:
            raise FsError(f"прочитано {len(image)} Б вместо {size}: дамп неполный")
        return image
    finally:
        if os.path.exists(path):
            os.unlink(path)


def board_wake(port):
    """Вывести плату из загрузчика сбросом через USB-JTAG.

    Тот же приём, что в `tools/reset.sh`: сброс делает сам `usb-reset`, а
    `read-mac` нужен только как повод поговорить с чипом (во flash ничего не
    пишется). Линии DTR/RTS при этом не участвуют — в этом и разница с
    `--after hard-reset`, после которого плата замолкает насовсем.
    """
    run_esptool(port, ["--before", "usb-reset", "--after", "no-reset",
                       "read-mac"])


def read_board_image(port, label):
    """Снять раздел с платы целиком."""
    if port is None:
        port = find_port()
    if port is None:
        raise FsError("плата не найдена: нет /dev/cu.usbmodem*, проверьте кабель")

    try:
        # Размер раздела берём из таблицы на самом чипе, а не из partitions.csv:
        # таблица могла быть переразмечена, а файл в репозитории — нет.
        table = board_read(port, PART_TABLE_ADDR, PART_TABLE_MAX)
        entries = parse_partition_table(table)
        chosen = next((e for e in entries if e["label"] == label), None)
        if chosen is None:
            raise FsError(
                f"в таблице разделов нет раздела {label}; есть: "
                + ", ".join(e["label"] for e in entries)
            )

        estimate = ESPTOOL_STARTUP_S + chosen["size"] / ESPTOOL_RATE
        print(f"# читаю {label} с {port}: {kib(chosen['size'])}, "
              f"примерно {human_time(estimate)}", file=sys.stderr)
        return board_read(port, chosen["address"], chosen["size"])
    finally:
        # Плату возвращаем к жизни в любом случае: и когда дамп удался, и когда
        # ошибка случилась уже после входа в загрузчик. Разбор идёт дальше на
        # маке и платы не касается, поэтому спешить некуда.
        try:
            board_wake(port)
        except FsError as exc:
            print(f"# плату не удалось вывести из загрузчика: {exc}",
                  file=sys.stderr)


def locate(image, label, base_override=None):
    """Где в дампе лежит раздел: (начало, конец).

    Дамп снимают двумя способами — целиком (тогда раздел ищется по таблице)
    или только разделом (тогда смещение нулевое). Различаем по содержимому,
    а не по размеру: урезанный полный дамп тоже бывает.
    """
    if base_override is not None:
        return base_override, len(image)

    if len(image) > PART_TABLE_ADDR + PART_TABLE_MAX:
        table = image[PART_TABLE_ADDR:PART_TABLE_ADDR + PART_TABLE_MAX]
        for entry in parse_partition_table(table):
            if entry["label"] != label:
                continue
            end = entry["address"] + entry["size"]
            if end > len(image):
                raise FsError(
                    f"раздел {label} кончается на {end:#x}, а дамп "
                    f"{len(image):#x}: дамп обрезан"
                )
            return entry["address"], end

    if len(image) >= BLOCK * SUPER_SECTORS:
        try:
            parse_super(Device(image, 0))
            return 0, len(image) // BLOCK * BLOCK
        except FsError:
            pass

    # Полный дамп, но таблица не прочиталась: ищем суперблок по известным
    # адресам разделов данных.
    for address in (0x210000, 0x10000):
        if address + BLOCK * SUPER_SECTORS > len(image):
            continue
        try:
            parse_super(Device(image[address:], address))
        except FsError:
            continue
        return address, len(image)

    raise FsError(
        f"раздел {label} в дампе не найден: ни таблицы разделов, ни суперблока"
    )


# ---------------------------------------------------------------------------
# Точка входа
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Разобрать ФС BFS в разделе flash — с платы или из дампа.",
        epilog="Дамп снимается один раз, дальше разбор мгновенный: --save и --read.",
    )
    parser.add_argument("--read", metavar="ФАЙЛ",
                        help="дамп flash или раздела вместо чтения с платы")
    parser.add_argument("--save", metavar="ФАЙЛ",
                        help="куда сохранить снятый дамп (по умолчанию временный)")
    parser.add_argument("-p", "--port", help="порт платы, по умолчанию ищется сам")
    parser.add_argument("--part", default="storage",
                        help="метка раздела (по умолчанию storage)")
    parser.add_argument("--base", type=lambda s: int(s, 0), metavar="СМЕЩЕНИЕ",
                        help="смещение раздела в дампе, если не определяется")
    parser.add_argument("-l", "--list", metavar="ПУТЬ", nargs="?", const="/",
                        help="показать дерево от пути (по умолчанию от корня)")
    parser.add_argument("-c", "--cat", metavar="ПУТЬ",
                        help="вывести содержимое файла в stdout")
    parser.add_argument("-o", "--extract", nargs=2, metavar=("ФАЙЛ", "ПУТЬ"),
                        help="выгрузить файл с платы на мак")
    parser.add_argument("--super", action="store_true",
                        help="суперблок, inode и сверка блоков")
    args = parser.parse_args()

    try:
        if args.read:
            try:
                with open(args.read, "rb") as handle:
                    image = handle.read()
            except FileNotFoundError:
                raise FsError(f"файл дампа не найден: {args.read}") from None
            except OSError as exc:
                raise FsError(f"не прочитать {args.read}: {exc.strerror}") from None
            start, end = locate(image, args.part, args.base)
            print(f"# дамп {args.read}: раздел {args.part} с {start:#x}, "
                  f"{kib(end - start)}", file=sys.stderr)
        else:
            if args.base is not None:
                raise FsError("--base имеет смысл только вместе с --read")
            image = read_board_image(args.port, args.part)
            start, end = 0, len(image)

        if args.save:
            with open(args.save, "wb") as handle:
                handle.write(image)
            print(f"# дамп сохранён: {args.save} ({kib(len(image))})",
                  file=sys.stderr)

        fs = Fs(Device(image[start:end], start))
        root, items, seen, problems = collect(fs)

        if args.cat:
            ino = fs.resolve(args.cat)
            if fs.kind(ino) != "файл":
                raise FsError(f"{args.cat}: это {fs.kind(ino)}, а не файл")
            sys.stdout.buffer.write(fs.read_file(ino))
            sys.stdout.buffer.flush()
            return 0

        if args.extract:
            target, path = args.extract
            ino = fs.resolve(path)
            if fs.kind(ino) != "файл":
                raise FsError(f"{path}: это {fs.kind(ino)}, а не файл")
            data = fs.read_file(ino)
            with open(target, "wb") as handle:
                handle.write(data)
            print(f"{target}: {len(data)} Б из {path}", file=sys.stderr)
            return 0

        if args.super:
            show_super(fs, seen, problems)

        if args.list is not None:
            ino = fs.resolve(args.list)
            if fs.kind(ino) != "каталог":
                raise FsError(f"{args.list}: это {fs.kind(ino)}, а не каталог")
            print_tree(items, args.list)

        if not (args.super or args.list is not None):
            print(f"BFS, поколение {fs.super['generation']}, "
                  f"{fs.super['block_count']} блоков "
                  f"({kib(fs.super['block_count'] * BLOCK)}), "
                  f"корень — inode {fs.super['root_inode']}\n")
            print_tree(items, "/")
            print_problems(problems)

        return 0
    except FsError as exc:
        print(f"fsdump: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"fsdump: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
