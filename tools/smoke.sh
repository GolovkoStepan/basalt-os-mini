#!/usr/bin/env bash
# Проверка прошивки на живой плате: залить, прочитать лог, сверить строки.
#
# Зачем: CI проверяет только то, что код собирается. Всё остальное - адреса
# таблицы векторов, тики, паника - видно только в логе с платы, и до сих пор
# это сверялось глазами. Глазами признак успеха проверяют один раз, а скрипт -
# каждый раз, и он же не даёт «вроде работает».
#
#   ./tools/smoke.sh                # env:bare  (по умолчанию)
#   ./tools/smoke.sh bare-panic
#   ./tools/smoke.sh console
#
# Код возврата: 0 - все ожидаемые строки на месте, 1 - нет.

set -uo pipefail

cd "$(dirname "$0")/.."

ENV="${1:-bare}"

# Сколько секунд читать и что обязано быть в логе. Строки повторяют «признак
# успеха» из JOURNAL.md. Дублирование намеренное: журнал читает человек, а
# скрипту нужен список для grep. Если меняется вывод прошивки, правится оба
# места - иначе проверка начнёт врать.
case "$ENV" in
    bare)
        READ_SECONDS=12
        PATTERNS=(
            '[vectors] VECBASE'
            'совпадает'
            '[ld] своя карта памяти'
            'ready'
            'tick 1'
        )
        ;;
    bare-panic)
        # Падение на 5-й секунде, отчёт, пауза 3 с, сброс. Меньше 14 с читать
        # нельзя: не увидим ни паники, ни причины сброса.
        READ_SECONDS=16
        PATTERNS=(
            '[сбой] слот 9'
            'причина 6'
            'деление на ноль'
            'сброс через 3 с'
            'rst:0x3'
        )
        ;;
    console)
        READ_SECONDS=8
        PATTERNS=(
            'basalt-os console: проверка связи'
            'если видно это - плата жива'
        )
        ;;
    # env:hello сюда намеренно не входит. Это щуп для голого модуля: он печатает
    # один раз в первые полторы секунды после старта, и попадёт ли это в окно
    # чтения, зависит от того, когда хост успел открыть порт. Проверка, которая
    # проходит через раз, хуже отсутствия проверки. Заодно после заливки hello
    # чип перестал отвечать esptool до ручного G0 - см. docs/board.md, грабли.
    *)
        echo "не знаю env '$ENV'. Есть: bare, bare-panic, console" >&2
        exit 1
        ;;
esac

echo "== env:$ENV: заливка"
if ! pio run -e "$ENV" -t upload >/tmp/basalt-smoke-upload.log 2>&1; then
    echo "заливка не удалась, хвост лога:" >&2
    tail -20 /tmp/basalt-smoke-upload.log >&2
    exit 1
fi

# После заливки чип перезагружается, и USB-устройство пересоздаётся: первое
# открытие порта часто попадает в этот момент и падает с ENXIO. Поэтому три
# попытки, а не одна; между ними пауза, чтобы порт успел появиться.
#
# Отдельный случай - чип остался в загрузчике: так бывает, если он попал туда
# вручную через G0. Тогда в логе видно "waiting for download", и заливка
# «успешна», а прошивка не запускается: esptool сбрасывает через RTS, а RTS на
# Cardputer к выводу сброса не подключён. Лечится сбросом через USB-JTAG -
# он вынесен в tools/reset.sh, потому что нужен не только здесь.
LOG=/tmp/basalt-smoke-$ENV.log
ATTEMPT=1
while :; do
    echo "== env:$ENV: чтение лога ${READ_SECONDS} с (попытка $ATTEMPT)"
    sleep 1
    if ./tools/serial-read.sh "$READ_SECONDS" >"$LOG" 2>/tmp/basalt-smoke-read.log; then
        FAILED=0
        for p in "${PATTERNS[@]}"; do
            if grep -Fq -- "$p" "$LOG"; then
                printf '  ok      %s\n' "$p"
            else
                printf '  НЕТ     %s\n' "$p"
                FAILED=1
            fi
        done
        [ "$FAILED" -eq 0 ] && break
    else
        echo "  порт не открылся (попытка $ATTEMPT)"
    fi

    if grep -Fq 'waiting for download' "$LOG" 2>/dev/null; then
        echo "  чип в загрузчике - сбрасываю через USB-JTAG"
        ./tools/reset.sh >/dev/null 2>&1 || true
    fi

    if [ "$ATTEMPT" -ge 3 ]; then
        echo
        echo "лог целиком ($LOG):" >&2
        cat "$LOG" >&2 2>/dev/null
        echo "чтение порта: см. /tmp/basalt-smoke-read.log" >&2
        echo "smoke не прошёл" >&2
        exit 1
    fi
    ATTEMPT=$((ATTEMPT + 1))
done

echo "smoke прошёл: лог $LOG"
