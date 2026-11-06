// Драйвер ST7789 на 8-битной параллельной шине (i80), через GPIO.
//
// ПОЧЕМУ НЕ esp_lcd, ХОТЯ 05.11.2026 РЕШИЛИ «БЕРЁМ ИЗ IDF».
//
// Готовый драйвер i80 из IDF работает на прерываниях: esp_lcd_new_i80_bus()
// ставит обработчик, а esp_lcd_panel_io_tx_color() включает его и сразу
// возвращается - «передача идёт в фоне». У нас прерывания пока не
// обрабатываются вообще: своя таблица векторов (arch/xtensa/vectors.S) все
// уровни 1..5 ведёт в панику. Значит первый же flush уронил бы систему, и не
// потому, что драйвер плохой, а потому, что в ОС ещё нет слоя прерываний -
// он придёт вместе с планировщиком.
//
// Вторая причина мельче, но того же рода: esp_lcd_new_panel_st7789() в
// init/reset вызывает vTaskDelay(), а планировщика FreeRTOS у нас нет.
//
// ЧТО ИЗ ЭТОГО СЛЕДУЕТ.
//
// Шина сделана вручную, но не «по регистрам LCD_CAM»: i80 - это восемь линий
// данных и такт WR, то есть обычный параллельный порт. Пишем в GPIO_OUT1_*
// напрямую, панель забирает байт по фронту WR. Ни DMA, ни очереди транзакций,
// ни прерываний - flush просто блокирует вызывающего на время передачи.
// Цена: процессор занят ~10 мс на полный кадр, и картинку нельзя готовить,
// пока идёт передача. Для текстового режима с dirty-строками это неважно, а
// когда появится планировщик, реализацию под lcd.h можно заменить на esp_lcd
// без единой правки выше интерфейса.
//
// ЧТО МЫ ВСЁ-ТАКИ НЕ ПИШЕМ САМИ. Начальную последовательность ST7789: она
// короткая (SWRESET, SLPOUT, COLMOD, MADCTL, DISPON), но значения в ней -
// знание о конкретном стекле, и берутся они из документа платы, а не из
// головы. Команды вынесены в lcd_init с ссылкой на даташит.

#include "lcd.h"

#include <stddef.h>
#include <stdint.h>

#include "board_pins.h"
#include "pin_gpio.h"

#include "esp_attr.h"
#include "esp_rom_sys.h"

// Все линии данных обязаны лежать в GPIO32..48: цикл заливки пишет только
// GPIO_OUT1_*. Если распиновку поменяют, это должно упасть на сборке, а не
// превратиться в «экран показывает мусор».
#if PIN_DISP_D0 < 32 || PIN_DISP_D1 < 32 || PIN_DISP_D2 < 32 || PIN_DISP_D3 < 32 || \
    PIN_DISP_D4 < 32 || PIN_DISP_D5 < 32 || PIN_DISP_D6 < 32 || PIN_DISP_D7 < 32
#error "Линии данных экрана должны быть в GPIO32..48 (регистры GPIO_OUT1_*)"
#endif

// Бит такта WR - того единственного пина, который остался в GPIO0..31.
#define WR_BIT (1u << (PIN_DISP_WR & 31u))

// Порядок линий данных: индекс массива - номер бита в байте, значение - пин.
static const uint8_t kDataPins[8] = {
    PIN_DISP_D0, PIN_DISP_D1, PIN_DISP_D2, PIN_DISP_D3,
    PIN_DISP_D4, PIN_DISP_D5, PIN_DISP_D6, PIN_DISP_D7,
};

// Готовые маски на каждый из 256 байт. Считаются один раз при инициализации:
// в горячем цикле дорога каждая инструкция, а раскладывать биты по пинам
// каждый раз - это восемь проверок на байт вместо двух записей в регистр.
static uint32_t s_set[256];
static uint32_t s_clr[256];

static bool s_ready;

int lcd_width(void)
{
    return DISP_WIDTH;
}

int lcd_height(void)
{
    return DISP_HEIGHT;
}

static void build_masks(void)
{
    uint32_t data_mask = 0;
    for (int bit = 0; bit < 8; ++bit) {
        data_mask |= 1u << (kDataPins[bit] - 32);
    }

    for (unsigned value = 0; value < 256u; ++value) {
        uint32_t set = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if (value & (1u << bit)) {
                set |= 1u << (kDataPins[bit] - 32);
            }
        }
        s_set[value] = set;
        s_clr[value] = data_mask & ~set;
    }
}

// Пауза между фронтами WR и сменой данных. Панель держит не больше ~15 МГц
// (t_cyc не меньше 66 нс), а четыре записи в регистры при 240 МГц
// укладываются в ~20 нс: без паузы байты ушли бы вдвое быстрее паспорта.
//
// Замерено на живой плате 06.11.2026: полный кадр 320x170 уходит за 32.1 мс,
// это 217 600 байт и ~147 нс на байт, то есть такт WR около 6.8 МГц -
// до предела панели остаётся двукратный запас. Число пауз увеличивать, чтобы
// «ускорить», бессмысленно: почти всё время уходит не на них, а на чтение
// пикселя из PSRAM и на подготовку масок. Быстрее - только DMA.
static inline void wr_settle(void)
{
    __asm__ volatile("nop\n\tnop\n\tnop");
}

// Один байт в шину. Данные выставляются до фронта WR (панель забирает их по
// переднему фронту), после фронта WR снова опускается: следующая смена данных
// должна произойти, пока такт низкий.
static inline void lcd_write_byte(uint8_t value)
{
    REG_WRITE(GPIO_OUT1_W1TS_REG, s_set[value]);
    REG_WRITE(GPIO_OUT1_W1TC_REG, s_clr[value]);
    wr_settle();
    REG_WRITE(GPIO_OUT_W1TS_REG, WR_BIT);
    REG_WRITE(GPIO_OUT_W1TC_REG, WR_BIT);
    wr_settle();
}

static void lcd_cmd(uint8_t cmd)
{
    pin_gpio_write(PIN_DISP_DC, false);
    lcd_write_byte(cmd);
}

static void lcd_data(const uint8_t* data, size_t size)
{
    pin_gpio_write(PIN_DISP_DC, true);
    for (size_t i = 0; i < size; ++i) {
        lcd_write_byte(data[i]);
    }
}

static void lcd_set_window(int x, int y, int w, int h)
{
    // Смещение панели прибавляется здесь, а не к координатам вызывающего:
    // тот мыслит в пикселях видимой области, а невидимые столбцы памяти -
    // забота драйвера.
    const int x0 = x + DISP_X_OFFSET;
    const int y0 = y + DISP_Y_OFFSET;

    const uint8_t caset[4] = {
        (uint8_t)((unsigned)x0 >> 8), (uint8_t)x0,
        (uint8_t)((unsigned)(x0 + w - 1) >> 8), (uint8_t)(x0 + w - 1),
    };
    lcd_cmd(0x2A); // CASET: столбцы
    lcd_data(caset, sizeof(caset));

    const uint8_t raset[4] = {
        (uint8_t)((unsigned)y0 >> 8), (uint8_t)y0,
        (uint8_t)((unsigned)(y0 + h - 1) >> 8), (uint8_t)(y0 + h - 1),
    };
    lcd_cmd(0x2B); // RASET: строки
    lcd_data(raset, sizeof(raset));

    lcd_cmd(0x2C); // RAMWR: дальше идут пиксели
}

void lcd_init(void)
{
    // Питание панели - раньше всего остального. На этой плате экран и
    // подсветка питаются отдельной линией: без неё все команды уходят в
    // никуда, а выглядит это как «прошивка работает, экран чёрный».
    pin_gpio_output(PIN_POWER_ON, true);
    esp_rom_delay_us(10000);

    // RD высокий: чтение из панели не используем, а низкий уровень включил бы
    // её выходы данных - восемь линий начали бы спорить с нашими.
    pin_gpio_output(PIN_DISP_RD, true);
    pin_gpio_output(PIN_DISP_CS, false);
    pin_gpio_output(PIN_DISP_WR, false);
    pin_gpio_output(PIN_DISP_DC, false);
    pin_gpio_output(PIN_DISP_RST, true);
    pin_gpio_output(PIN_DISP_BL, false);
    for (int i = 0; i < 8; ++i) {
        pin_gpio_output(kDataPins[i], false);
    }

    build_masks();

    // Аппаратный сброс: импульс не короче 10 мкс, затем пауза 120 мс - столько
    // ST7789 просит до первой команды после снятия сброса.
    pin_gpio_output(PIN_DISP_RST, false);
    esp_rom_delay_us(1000);
    pin_gpio_output(PIN_DISP_RST, true);
    esp_rom_delay_us(120000);

    lcd_cmd(0x01); // SWRESET
    esp_rom_delay_us(120000);

    lcd_cmd(0x11); // SLPOUT: выйти из спящего режима
    esp_rom_delay_us(120000);

    // COLMOD: 16 бит на пиксель, то есть RGB565 - ровно то, что лежит в
    // кадровом буфере.
    lcd_cmd(0x3A);
    const uint8_t colmod = 0x55;
    lcd_data(&colmod, 1);

    // MADCTL: ориентация и порядок цветов, замеренные на этой панели.
    lcd_cmd(0x36);
    const uint8_t madctl = DISP_MADCTL;
    lcd_data(&madctl, 1);

#if DISP_INVERT_COLORS
    lcd_cmd(0x21); // INVON
#endif

    lcd_cmd(0x13); // NORON: обычный режим (не частичный)
    lcd_cmd(0x29); // DISPON
    esp_rom_delay_us(20000);

    s_ready = true;
}

void lcd_backlight(bool on)
{
    pin_gpio_write(PIN_DISP_BL, on);
}

// IRAM_ATTR: цикл читает кадровый буфер из PSRAM, который идёт через тот же
// кэш, что и код во flash. Код, лежащий во flash, в этом месте вытесняется
// данными и начинает ждать сам себя - передача растягивается без пользы.
void IRAM_ATTR lcd_flush(int x, int y, int w, int h, const uint16_t* pixels)
{
    // Прямоугольник проверяем, а не обрезаем: молча укороченная заливка
    // сдвинула бы весь рисунок, и ошибку искали бы в шрифте.
    if (!s_ready || pixels == NULL || w <= 0 || h <= 0 ||
        x < 0 || y < 0 || x + w > DISP_WIDTH || y + h > DISP_HEIGHT) {
        return;
    }

    lcd_set_window(x, y, w, h);

    // DC высокий на всё время пикселей: это данные, а не команды.
    pin_gpio_write(PIN_DISP_DC, true);

    // Старший байт первым: ST7789 принимает RGB565 как два байта, и старший
    // идёт раньше. В памяти uint16_t лежит младшим байтом вперёд, поэтому
    // порядок здесь приходится разворачивать - при копировании «как есть»
    // красный и синий меняются местами.
    const size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; ++i) {
        const uint16_t value = pixels[i];
        lcd_write_byte((uint8_t)(value >> 8));
        lcd_write_byte((uint8_t)value);
    }
}
