// Приём XMODEM: пакеты, CRC, ACK/NAK. Подробности протокола - в xmodem.h.
//
// Почему CRC, а не контрольная сумма. У классического XMODEM в конце пакета
// один байт суммы, и он пропускает часть ошибок; CRC-16 ловит их все, кроме
// намеренной подделки. Стоит это двух байт на пакет и цикла на 16 итераций -
// на фоне записи во flash незаметно. Начало передачи помечается 'C' (CRC),
// поэтому старый отправитель с NAK-рукопожатием сюда не пройдёт: он должен
// уметь CRC. Наш xsend.py умеет, как и lrzsz/`sx`.

#include "xmodem.h"

#include <stdbool.h>
#include <string.h>

#define XM_SOH 0x01u
#define XM_STX 0x02u
#define XM_EOT 0x04u
#define XM_ACK 0x06u
#define XM_NAK 0x15u
#define XM_CAN 0x18u
#define XM_CRC_REQ ((uint8_t)'C')

// Им дополняется последний пакет: в CP/M это была метка конца файла, в XMODEM
// осталась заполнителем. В файл этот байт попадать не должен - иначе `cat`
// показывал бы в конце мусор, а `rc`-файл получал бы ^Z последней строкой.
#define XM_PAD 0x1Au

#define XM_BLOCK_128 128u
#define XM_BLOCK_1K 1024u
#define XM_BLOCK_MAX XM_BLOCK_1K

// Пакет целиком: seq, ~seq, данные, CRC.
#define XM_PACKET_MAX (2u + XM_BLOCK_MAX + 2u)

// Хвост из 0x1A держим, пока не станет ясно, дополнение это или данные.
// Верхняя граница - один пакет плюс столько же, что пришло в новом пакете:
// если и следующий пакет сплошной 0x1A, лишнее уже точно не дополнение.
#define XM_HOLD_MAX (XM_BLOCK_MAX * 2u)

// Сколько раз просить начать и сколько раз повторять пакет. Числа не из
// стандарта: важен порядок - десятки, а не тысячи, чтобы оборванная связь
// заканчивалась ошибкой, а не вечным ожиданием.
#define XM_START_TRIES 5
#define XM_PACKET_TRIES 10

// Ожидание первого байта. На старте - щедрее: отправитель мог ещё не открыть
// порт; в середине передачи молчание в три секунды означает обрыв.
#define XM_START_TIMEOUT_MS 3000u
#define XM_PACKET_TIMEOUT_MS 3000u

// Одиночный CAN ничего не значит (это просто байт 0x18), отмена - только два
// подряд. Второй ждём недолго: если его нет, значит байт был случайным.
#define XM_CANCEL_TIMEOUT_MS 500u

typedef struct {
    const xmodem_io* io;

    uint8_t packet[XM_PACKET_MAX];
    uint8_t hold[XM_HOLD_MAX];
    size_t hold_len;
    uint32_t total;
} rx_state;

// Один приёмник на систему. Планировщика нет, вложенных приёмов не бывает;
// цена - несколько килобайт .bss вместо такого же минуса из стека шелла,
// который на плате всего 8 КБ.
static rx_state s_rx;

static uint16_t crc16(const uint8_t* data, size_t len)
{
    // CRC-16/XMODEM: полином 0x1021, начальное значение 0, без отражения.
    // Считается побитно: таблица на 256 значений заняла бы полкилобайта ради
    // скорости, которая на фоне flash не нужна.
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static int put_all(rx_state* rx, const uint8_t* data, size_t len)
{
    return rx->io->put(rx->io->ctx, data, len) == (int)len ? XMODEM_OK : XMODEM_ERR_IO;
}

static int put_byte(rx_state* rx, uint8_t byte)
{
    return put_all(rx, &byte, 1);
}

// Выбросить то, что успело прийти после испорченного пакета: иначе остаток
// старого пакета будет принят за начало нового, и поток разъедется молча.
static void flush_input(rx_state* rx)
{
    uint8_t junk[32];
    while (rx->io->get(rx->io->ctx, junk, sizeof(junk), 0) > 0) {
    }
}

// Ошибка, о которой надо сообщить отправителю: два CAN подряд останавливают
// lrzsz и наш xsend.py, иначе они продолжают ждать до своего таймаута.
static int give_up(rx_state* rx, int err)
{
    if (err != XMODEM_ERR_IO) {
        const uint8_t can[2] = {XM_CAN, XM_CAN};
        (void)put_all(rx, can, sizeof(can));
    }
    return err;
}

static int emit(rx_state* rx, const uint8_t* data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (rx->io->sink(rx->io->ctx, data, len) != 0) {
        return -1;
    }
    rx->total += (uint32_t)len;
    return 0;
}

// Отдать данные пакета, придержав хвост из 0x1A. Хвост нужен только для
// последнего пакета, но какой пакет последний, заранее неизвестно - узнаём по
// EOT. Поэтому 0x1A не пишется сразу, а ждёт первого байта, который не 0x1A.
// Если такой байт пришёл, значит придержанное - настоящие данные, и они уходят
// перед ним.
static int accept_block(rx_state* rx, const uint8_t* data, size_t len)
{
    size_t tail = 0;
    while (tail < len && data[len - 1u - tail] == XM_PAD) {
        ++tail;
    }

    const int rc = emit(rx, rx->hold, rx->hold_len);
    if (rc != 0) {
        return rc;
    }
    rx->hold_len = 0;

    if (emit(rx, data, len - tail) != 0) {
        return -1;
    }

    memcpy(rx->hold, data + (len - tail), tail);
    rx->hold_len = tail;

    // Сплошные 0x1A пакеты подряд: дополнение не может быть длиннее пакета,
    // значит самое начало придержанного - уже данные. Отдаём лишнее.
    if (rx->hold_len > XM_BLOCK_MAX) {
        const size_t extra = rx->hold_len - XM_BLOCK_MAX;
        if (emit(rx, rx->hold, extra) != 0) {
            return -1;
        }
        memmove(rx->hold, rx->hold + extra, XM_BLOCK_MAX);
        rx->hold_len = XM_BLOCK_MAX;
    }
    return 0;
}

int xmodem_receive(const xmodem_io* io, uint32_t* size)
{
    if (size != NULL) {
        *size = 0;
    }
    if (io == NULL || io->put == NULL || io->get == NULL || io->sink == NULL ||
        io->finish == NULL) {
        return XMODEM_ERR_ARG;
    }

    rx_state* const rx = &s_rx;
    memset(rx, 0, sizeof(*rx));
    rx->io = io;

    // Номера пакетов идут 1..255, потом 0. uint8_t переполняется сам, поэтому
    // ожидаемый номер - обычное приращение, без особого случая на 255.
    uint8_t expected = 1;
    uint32_t blocks = 0;
    int start_tries = 0;
    int packet_tries = 0;
    bool started = false;

    for (;;) {
        if (!started) {
            if (start_tries >= XM_START_TRIES) {
                return give_up(rx, XMODEM_ERR_TIMEOUT);
            }
            ++start_tries;
            if (put_byte(rx, XM_CRC_REQ) != XMODEM_OK) {
                return XMODEM_ERR_IO;
            }
        }

        uint8_t header = 0;
        const uint32_t timeout = started ? XM_PACKET_TIMEOUT_MS : XM_START_TIMEOUT_MS;
        if (rx->io->get(rx->io->ctx, &header, 1, timeout) != 1) {
            if (!started) {
                continue;  // ещё раз попросим CRC
            }
            if (++packet_tries > XM_PACKET_TRIES) {
                return give_up(rx, XMODEM_ERR_TIMEOUT);
            }
            flush_input(rx);
            if (put_byte(rx, XM_NAK) != XMODEM_OK) {
                return XMODEM_ERR_IO;
            }
            continue;
        }

        if (header == XM_CAN) {
            // Отмена - это два CAN подряд. Второй байт может оказаться началом
            // пакета; если его выбросить, поток разъедется, и потеря обнаружится
            // только по чужому номеру блока. Поэтому разбираем его как обычный
            // заголовок, а не как продолжение отмены.
            uint8_t next = 0;
            if (rx->io->get(rx->io->ctx, &next, 1, XM_CANCEL_TIMEOUT_MS) != 1) {
                continue;  // одиночный CAN - случайный байт
            }
            if (next == XM_CAN) {
                return XMODEM_ERR_CANCEL;
            }
            header = next;
        }

        if (header == XM_EOT) {
            // Хвост из 0x1A в файл не идёт: это дополнение последнего пакета.
            rx->hold_len = 0;

            // Дописываем придержанное до ACK: подтверждение должно означать
            // «файл лежит», а не «байты приняты в буфер».
            if (rx->io->finish(rx->io->ctx) != 0) {
                return give_up(rx, XMODEM_ERR_SINK);
            }
            if (put_byte(rx, XM_ACK) != XMODEM_OK) {
                return XMODEM_ERR_IO;
            }
            if (size != NULL) {
                *size = rx->total;
            }
            return XMODEM_OK;
        }

        if (header != XM_SOH && header != XM_STX) {
            // Не начало пакета. До старта это шум в линии: эхо набранной
            // команды, отчёты загрузчика. После старта - мусор в потоке.
            if (started) {
                if (++packet_tries > XM_PACKET_TRIES) {
                    return give_up(rx, XMODEM_ERR_PROTOCOL);
                }
                if (put_byte(rx, XM_NAK) != XMODEM_OK) {
                    return XMODEM_ERR_IO;
                }
            }
            continue;
        }

        // Заголовок увидели - передача началась. Дальше любое молчание уже
        // повод для NAK, а не для нового 'C'.
        started = true;

        const size_t len = (header == XM_STX) ? XM_BLOCK_1K : XM_BLOCK_128;
        const size_t want = 2u + len + 2u;
        const size_t got =
            (size_t)rx->io->get(rx->io->ctx, rx->packet, want, XM_PACKET_TIMEOUT_MS);
        if (got != want) {
            if (++packet_tries > XM_PACKET_TRIES) {
                return give_up(rx, XMODEM_ERR_TIMEOUT);
            }
            flush_input(rx);
            if (put_byte(rx, XM_NAK) != XMODEM_OK) {
                return XMODEM_ERR_IO;
            }
            continue;
        }

        const uint8_t seq = rx->packet[0];
        const uint8_t seq_inv = rx->packet[1];
        const uint8_t* const data = rx->packet + 2;
        const uint16_t sent_crc =
            (uint16_t)(((uint16_t)rx->packet[2u + len] << 8) | rx->packet[2u + len + 1u]);

        // Номер и его побитовая инверсия: если они не складываются в 0xFF,
        // сдвинулся уже сам заголовок, и данные не тем, чем кажутся.
        const bool seq_ok = ((uint8_t)(seq + seq_inv) == 0xFFu);
        const bool crc_ok = (crc16(data, len) == sent_crc);

        if (!seq_ok || !crc_ok) {
            if (++packet_tries > XM_PACKET_TRIES) {
                return give_up(rx, XMODEM_ERR_PROTOCOL);
            }
            if (put_byte(rx, XM_NAK) != XMODEM_OK) {
                return XMODEM_ERR_IO;
            }
            continue;
        }

        if (seq == expected) {
            if (accept_block(rx, data, len) != 0) {
                return give_up(rx, XMODEM_ERR_SINK);
            }
            expected = (uint8_t)(expected + 1u);
            ++blocks;
            packet_tries = 0;
            if (put_byte(rx, XM_ACK) != XMODEM_OK) {
                return XMODEM_ERR_IO;
            }
            continue;
        }

        if (blocks > 0 && seq == (uint8_t)(expected - 1u)) {
            // Повтор уже принятого пакета: наш ACK не дошёл. Данные второй раз
            // не пишем, но подтверждаем - иначе отправитель зациклится.
            packet_tries = 0;
            if (put_byte(rx, XM_ACK) != XMODEM_OK) {
                return XMODEM_ERR_IO;
            }
            continue;
        }

        // Чужой номер означает, что поток разъехался: дочитывать его нельзя,
        // продолжение запишет мусор в файл. Честнее остановиться.
        return give_up(rx, XMODEM_ERR_SEQUENCE);
    }
}

const char* xmodem_strerror(int rc)
{
    switch (rc) {
    case XMODEM_OK:
        return "готово";
    case XMODEM_ERR_ARG:
        return "приёмник XMODEM получил неполные аргументы";
    case XMODEM_ERR_IO:
        return "консоль не принимает и не отдаёт байты";
    case XMODEM_ERR_TIMEOUT:
        return "отправитель не отвечает";
    case XMODEM_ERR_PROTOCOL:
        return "пакеты приходят битыми";
    case XMODEM_ERR_SEQUENCE:
        return "нарушен порядок пакетов";
    case XMODEM_ERR_CANCEL:
        return "передача отменена отправителем";
    case XMODEM_ERR_SINK:
        return "не удалось записать файл";
    default:
        return "неизвестная ошибка XMODEM";
    }
}
