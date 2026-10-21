#!/usr/bin/env bash
# Проверка собранного образа env:bare по таблице символов. Плата не нужна.
#
# Зачем: часть обещаний A0 невидима компилятору и молча ломается - таблица
# векторов перестала быть выровнена на 1 КБ, карта памяти подменилась чужой,
# куча оказалась вне DRAM. Всё это проявляется уже на плате, и искать причину
# там дорого. Здесь те же обещания проверяются за секунду.
#
#   ./tools/check-image.sh                      # .pio/build/bare/firmware.elf
#   ./tools/check-image.sh путь/к/firmware.elf
#
# Код возврата: 0 - всё сошлось, 1 - что-то не так.

set -uo pipefail

cd "$(dirname "$0")/.."

ELF="${1:-.pio/build/bare/firmware.elf}"

if [ ! -f "$ELF" ]; then
    echo "нет файла $ELF - сначала: pio run -e bare" >&2
    exit 1
fi

NM=""
for c in "$HOME"/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm; do
    [ -x "$c" ] && NM="$c"
done
if [ -z "$NM" ]; then
    echo "не найден xtensa-esp32s3-elf-nm в ~/.platformio/packages" >&2
    exit 1
fi

FAILED=0
SYMS=$("$NM" -n "$ELF")

# Адрес символа; пусто, если символа нет.
addr_of() {
    echo "$SYMS" | awk -v want="$1" '$3 == want { print $1; exit }'
}

check() {
    local what="$1" ok="$2" detail="$3"
    if [ "$ok" = "1" ]; then
        printf '  ok   %s (%s)\n' "$what" "$detail"
    else
        printf '  БЕДА %s (%s)\n' "$what" "$detail"
        FAILED=1
    fi
}

# Границы областей из ld/memory.ld. Дублируются здесь намеренно: если карту
# поменяют, проверка обязана это заметить, а не подстроиться.
IRAM_START=$((0x40374000))
IRAM_END=$((0x403CB700))
DRAM_START=$((0x3FC88000))
DRAM_END=$((0x3FCDB700))
DIRAM_I_START=$((0x40378000))

echo "образ: $ELF"

# 1. Образ связан нашей картой, а не сгенерированной IDF.
VERSION=$(addr_of basalt_memory_map_version)
check "карта памяти своя" "$([ -n "$VERSION" ] && echo 1 || echo 0)" \
      "basalt_memory_map_version=${VERSION:-нет}"

# 2. Таблица векторов: в IRAM и выровнена на 1 КБ. Младшие 10 бит адреса VECBASE
#    не хранит, поэтому невыровненная таблица тихо съедет на границу блока.
VEC=$(addr_of basalt_vectors)
if [ -n "$VEC" ]; then
    V=$((16#$VEC))
    check "векторы в IRAM" "$([ "$V" -ge "$IRAM_START" ] && [ "$V" -lt "$IRAM_END" ] && echo 1 || echo 0)" \
          "0x$VEC"
    check "векторы на границе 1 КБ" "$([ $((V % 1024)) -eq 0 ] && echo 1 || echo 0)" \
          "0x$VEC % 1024 = $((V % 1024))"
else
    check "векторы в IRAM" 0 "символа basalt_vectors нет"
fi

# 3. Куча начинается внутри DRAM и не залезает в чужое.
HEAP=$(addr_of _heap_start)
if [ -n "$HEAP" ]; then
    H=$((16#$HEAP))
    check "начало кучи в DRAM" "$([ "$H" -ge "$DRAM_START" ] && [ "$H" -lt "$DRAM_END" ] && echo 1 || echo 0)" \
          "0x$HEAP"
else
    check "начало кучи в DRAM" 0 "символа _heap_start нет"
fi

# 4. Общий блок кода и данных начинается там, где сходятся два вида на память:
#    0x40378000 и 0x3FC88000 - один и тот же байт.
DIRAM=$(addr_of _diram_i_start)
check "_diram_i_start = 0x40378000" \
      "$([ -n "$DIRAM" ] && [ $((16#$DIRAM)) -eq $DIRAM_I_START ] && echo 1 || echo 0)" \
      "0x${DIRAM:-нет}"

if [ "$FAILED" -ne 0 ]; then
    echo "проверка образа не прошла" >&2
    exit 1
fi
echo "образ соответствует карте памяти"
