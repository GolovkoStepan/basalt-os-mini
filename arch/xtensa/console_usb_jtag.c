// Драйвер USB Serial/JTAG по регистрам периферии.
//
// База 0x6003_8000. Всё общение идёт через EP1:
//   EP1_REG      - байт для чтения (когда пришёл) и байт для записи
//   EP1_CONF_REG - два флага состояния:
//                    bit 1 SERIAL_IN_EP_DATA_FREE  - можно писать
//                    bit 2 SERIAL_OUT_EP_DATA_AVAIL - есть что читать
//                  и bit 0 WR_DONE - отправить накопленное хосту
//
// Прерывания не используем: на A0 нет планировщика, опрос проще и не может
// потерять обработчик. Прерывания понадобятся, когда появится планировщик.

#include "console_usb_jtag.h"

#include "soc/usb_serial_jtag_reg.h"

// Предел ожидания в итерациях. Важно не число, а то, что оно есть: без него
// первая же запись при закрытом терминале подвесила бы ядро намертво, и
// отличить «завис» от «не залилось» было бы нельзя.
#define TX_SPIN_LIMIT 200000

void basalt_console_init(void)
{
    // Сбрасываем FIFO и снимаем возможные зависшие флаги прерываний.
    // Работает и если периферию уже поднял загрузчик: повторная инициализация
    // безопасна, потому что мы не полагаемся на предыдущее состояние.
    WRITE_PERI_REG(USB_SERIAL_JTAG_INT_CLR_REG, 0xFFFFFFFFu);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_EP1_CONF_REG,
                      USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE);
}

static bool tx_ready(void)
{
    return (READ_PERI_REG(USB_SERIAL_JTAG_EP1_CONF_REG) &
            USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE) != 0;
}

size_t basalt_console_write(const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t written = 0;

    while (written < len) {
        uint32_t spin = 0;
        while (!tx_ready()) {
            if (++spin > TX_SPIN_LIMIT) {
                // Хост не забирает: отдаём что успели, ядро продолжает жить.
                return written;
            }
        }
        WRITE_PERI_REG(USB_SERIAL_JTAG_EP1_REG, p[written]);
        ++written;

        // FIFO имеет 64 байта; отправляем, когда он полон, и в конце вызова,
        // чтобы данные ушли сразу, а не с задержкой до следующей записи.
        if ((written % 64) == 0 || written == len) {
            SET_PERI_REG_MASK(USB_SERIAL_JTAG_EP1_CONF_REG,
                              USB_SERIAL_JTAG_WR_DONE);
        }
    }
    return written;
}

size_t basalt_console_read(void* data, size_t len)
{
    uint8_t* p = (uint8_t*)data;
    size_t got = 0;

    while (got < len) {
        if ((READ_PERI_REG(USB_SERIAL_JTAG_EP1_CONF_REG) &
             USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL) == 0) {
            break;
        }
        p[got] = (uint8_t)(READ_PERI_REG(USB_SERIAL_JTAG_EP1_REG) & 0xFF);
        ++got;
    }
    return got;
}
