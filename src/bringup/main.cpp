// Щуп bringup: проверка железа Cardputer без ОС.
//
// Это не A0. A0 начинается со своей точки входа и своего линкер-скрипта; сюда
// же собрано всё, что нужно знать до неё и что нельзя выяснить чтением
// документации: жива ли консоль через USB Serial/JTAG, как панель отвечает на
// голые регистры ST7789, что читается с матрицы клавиатуры и на какой частоте
// держится SPI. Замеры печатаются в serial, а не остаются в переписке.
//
// Запускать: pio run -e bringup -t upload, потом tools/serial-read.sh 20.

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "board_pins.h"

// ---------------------------------------------------------------------------
// SPI + ST7789
// ---------------------------------------------------------------------------

static spi_device_handle_t s_spi = nullptr;
static uint32_t s_spi_hz = 0;

// Кадр целиком, 240x135x2 = 64.8 КБ. Для A1 такой буфер не нужен (рисуем
// полосой), но именно он даёт честный замер пропускной способности шины и
// заодно отвечает на вопрос, помещается ли кадр в SRAM вообще.
static uint16_t s_frame[DISP_WIDTH * DISP_HEIGHT];

static void disp_cmd(uint8_t cmd, const uint8_t* data, size_t len)
{
    spi_transaction_t t = {};
    digitalWrite(PIN_DISP_DC, LOW);
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_polling_transmit(s_spi, &t);
    if (len > 0) {
        digitalWrite(PIN_DISP_DC, HIGH);
        t.length = len * 8;
        t.tx_buffer = data;
        spi_device_polling_transmit(s_spi, &t);
    }
}

// Частота задаётся здесь, а не в platformio.ini: перебрать 40/60/80 МГц нужно
// в одном прогоне. Ветка без remove/free - первый вызов, дальше только смена
// устройства (шина остаётся инициализированной).
static void disp_spi_setup(uint32_t hz)
{
    digitalWrite(PIN_DISP_CS, HIGH);
    if (s_spi != nullptr) {
        spi_bus_remove_device(s_spi);
        s_spi = nullptr;
    } else {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = PIN_DISP_MOSI;
        bus.miso_io_num = -1;               // панель трёхпроводная
        bus.sclk_io_num = PIN_DISP_SCK;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = sizeof(s_frame);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    }

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = hz;
    dev.mode = 0;                           // вендор использует режим 0
    dev.spics_io_num = -1;                  // CS держим сами: он общий на кадр
    dev.queue_size = 4;                     // полосы кадра копятся в очереди
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &s_spi));

    s_spi_hz = hz;
    digitalWrite(PIN_DISP_CS, LOW);
}

static void disp_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const uint8_t caset[4] = {
        (uint8_t)((x0 + DISP_OFFSET_X) >> 8), (uint8_t)(x0 + DISP_OFFSET_X),
        (uint8_t)((x1 + DISP_OFFSET_X) >> 8), (uint8_t)(x1 + DISP_OFFSET_X),
    };
    const uint8_t raset[4] = {
        (uint8_t)((y0 + DISP_OFFSET_Y) >> 8), (uint8_t)(y0 + DISP_OFFSET_Y),
        (uint8_t)((y1 + DISP_OFFSET_Y) >> 8), (uint8_t)(y1 + DISP_OFFSET_Y),
    };
    disp_cmd(0x2A, caset, sizeof(caset));
    disp_cmd(0x2B, raset, sizeof(raset));
    disp_cmd(0x2C, nullptr, 0);
}

// Кадр одной DMA-транзакцией не отправить: у S3 потолок - 1<<18 бит
// (32 768 Б), а кадр 64 800 Б. Режем на куски по 15 строк (7200 Б, 57600 бит):
// 135 строк делятся ровно, поэтому все транзакции одной длины и замер не
// зависит от «последней неполной». Очередь на 4: пока панель выплёвывает один
// кусок, следующие уже лежат в драйвере.
static constexpr int kChunkRows = 15;
static constexpr int kMaxChunks = DISP_HEIGHT / kChunkRows + 1;

// Отправляет rows строк начиная с top_row. Возвращает время в микросекундах,
// включая установку окна: для A1 важна именно эта цифра - «нарисовать строку
// текста», а не отдельно чистая передача байт.
static uint32_t disp_send_rows(int top_row, int rows)
{
    disp_window(0, top_row, DISP_WIDTH - 1, top_row + rows - 1);
    digitalWrite(PIN_DISP_DC, HIGH);

    static spi_transaction_t trans[kMaxChunks];
    int chunks = 0;
    const int64_t t0 = esp_timer_get_time();
    for (int y = 0; y < rows; y += kChunkRows) {
        const int chunk = (rows - y < kChunkRows) ? (rows - y) : kChunkRows;
        trans[chunks] = {};
        trans[chunks].length = DISP_WIDTH * chunk * 2 * 8;
        trans[chunks].tx_buffer = &s_frame[(top_row + y) * DISP_WIDTH];
        ESP_ERROR_CHECK(spi_device_queue_trans(s_spi, &trans[chunks], portMAX_DELAY));
        ++chunks;
    }
    for (int i = 0; i < chunks; ++i) {
        spi_transaction_t* done = nullptr;
        ESP_ERROR_CHECK(spi_device_get_trans_result(s_spi, &done, portMAX_DELAY));
    }
    return (uint32_t)(esp_timer_get_time() - t0);
}

struct InitCmd {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[14];
    uint16_t delay_ms;
};

// Последовательность снята с M5GFX Panel_ST7789::getInitCommands - это то, что
// реально исполнялось на этой панели. Порядок не переставляем: PORCTRL и
// FRCTRL2 в апстриме отключены, у разных партий панели разные значения.
static const InitCmd kInit[] = {
    {0xB7, 1, {0x35}, 0},                                   // GCTRL
    {0xBB, 1, {0x28}, 0},                                   // VCOMS
    {0xC0, 1, {0x0C}, 0},                                   // LCMCTRL
    {0xC2, 2, {0x01, 0xFF}, 0},                             // VDVVRHEN
    {0xC3, 1, {0x10}, 0},                                   // VRHS
    {0xC4, 1, {0x20}, 0},                                   // VDVSET
    {0xD0, 2, {0xA4, 0xA1}, 0},                             // PWCTRL1
    {0xB0, 2, {0x00, 0xC0}, 0},                             // RAMCTRL
    {0xE0, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32,
                0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17}, 0}, // PVGAMCTRL
    {0xE1, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31,
                0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E}, 0}, // NVGAMCTRL
    {0x11, 0, {0}, 130},                                    // SLPOUT
    {0x34, 0, {0}, 0},                                      // IDMOFF (tearing off)
    {0x3A, 1, {0x55}, 0},                                   // COLMOD: RGB565
    {0x36, 1, {DISP_MADCTL}, 0},                            // MADCTL: rotation 1
    {0x21, 0, {0}, 10},                                     // INVON: панель инверсная
    {0x29, 0, {0}, 20},                                     // DISPON
};

static void disp_init(void)
{
    // Аппаратный сброс: RST на G33, тайминги из даташита.
    digitalWrite(PIN_DISP_RST, HIGH);
    delay(10);
    digitalWrite(PIN_DISP_RST, LOW);
    delay(20);
    digitalWrite(PIN_DISP_RST, HIGH);
    delay(120);

    for (const InitCmd& c : kInit) {
        disp_cmd(c.cmd, c.data, c.len);
        if (c.delay_ms) {
            delay(c.delay_ms);
        }
    }
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Тестовая картинка: 8 вертикальных полос + белая рамка в 1 пиксель.
// Рамка - главный индикатор: при неверных смещениях она обрезается или
// подъезжает, а при неверном MADCTL картинка окажется повёрнутой или
// зеркальной. Полосы проверяют RGB/BGR (красный должен быть красным).
static void draw_test_pattern(void)
{
    static const uint16_t bars[8] = {
        rgb565(255, 0, 0),   rgb565(0, 255, 0), rgb565(0, 0, 255),
        rgb565(255, 255, 255), rgb565(0, 0, 0), rgb565(255, 255, 0),
        rgb565(0, 255, 255), rgb565(255, 0, 255),
    };
    const uint16_t border = rgb565(255, 255, 255);

    for (int y = 0; y < DISP_HEIGHT; ++y) {
        for (int x = 0; x < DISP_WIDTH; ++x) {
            bool edge = (x == 0) || (x == DISP_WIDTH - 1) ||
                        (y == 0) || (y == DISP_HEIGHT - 1);
            s_frame[y * DISP_WIDTH + x] = edge ? border : bars[x / 30];
        }
    }
}

// ---------------------------------------------------------------------------
// Клавиатура
// ---------------------------------------------------------------------------

static const uint8_t kKeyAddr[3] = {PIN_KEY_ADDR_0, PIN_KEY_ADDR_1, PIN_KEY_ADDR_2};
static const uint8_t kKeyRow[KEY_COLS] = {
    PIN_KEY_ROW_0, PIN_KEY_ROW_1, PIN_KEY_ROW_2, PIN_KEY_ROW_3,
    PIN_KEY_ROW_4, PIN_KEY_ROW_5, PIN_KEY_ROW_6,
};

static void keys_init(void)
{
    for (uint8_t p : kKeyAddr) {
        pinMode(p, OUTPUT);
        digitalWrite(p, LOW);
    }
    for (uint8_t p : kKeyRow) {
        pinMode(p, INPUT_PULLUP);
    }
}

// Карта нажатий: бит (строка * 7 + столбец). Раскладка и слой Fn - задача A2,
// здесь нужен только факт «железо отдаёт все 56 позиций».
static uint64_t keys_scan(void)
{
    uint64_t map = 0;
    for (int line = 0; line < KEY_LINES; ++line) {
        digitalWrite(PIN_KEY_ADDR_0, (line & 1) ? HIGH : LOW);
        digitalWrite(PIN_KEY_ADDR_1, (line & 2) ? HIGH : LOW);
        digitalWrite(PIN_KEY_ADDR_2, (line & 4) ? HIGH : LOW);
        delayMicroseconds(10);      // демультиплексору и RC на входах - время
        for (int col = 0; col < KEY_COLS; ++col) {
            if (digitalRead(kKeyRow[col]) == LOW) {
                map |= 1ULL << (line * KEY_COLS + col);
            }
        }
    }
    return map;
}

// Фактическая частота печатается не из вредности: 60 МГц на S3 недостижимы -
// делитель считается от 80 МГц, ближайшая снизу ступень 40. Без этой цифры
// «запросили 60, получили как 40» выглядит как ошибка замера.
static void report_transfer(const char* what, uint32_t requested_hz,
                            uint32_t bytes, uint32_t us)
{
    int actual_khz = 0;
    spi_device_get_actual_freq(s_spi, &actual_khz);
    const double actual_hz = actual_khz * 1000.0;
    const double mb_s = us ? (double)bytes / (double)us : 0.0;   // Б/мкс = МБ/с
    const double ideal_ms = (double)bytes * 8.0 / actual_hz * 1000.0;
    Serial.printf("[spi] %-13s просили %2lu, факт %5.1f МГц: %6.3f мс, "
                  "%5.2f МБ/с (идеал %6.3f мс)\n",
                  what, (unsigned long)(requested_hz / 1000000),
                  actual_hz / 1e6, us / 1000.0, mb_s, ideal_ms);
}

// ---------------------------------------------------------------------------

static void print_boot_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    Serial.println();
    Serial.println("==============================================");
    Serial.println("basalt-os bringup: щуп железа Cardputer (K132)");
    Serial.println("==============================================");
    Serial.printf("chip      : ESP32-S3, %d ядер, rev v%d.%d\n",
                  info.cores, info.revision / 100, info.revision % 100);
    Serial.printf("cpu       : %lu МГц\n", (unsigned long)getCpuFrequencyMhz());
    Serial.printf("flash     : %lu МБ\n",
                  (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));
    Serial.printf("psram     : %s\n", ESP.getPsramSize() ? "есть" : "НЕТ (как и ожидалось)");
    Serial.printf("reset     : %d (%s)\n", (int)esp_reset_reason(),
                  esp_reset_reason() == ESP_RST_POWERON ? "power-on"
                  : esp_reset_reason() == ESP_RST_USB ? "USB"
                  : esp_reset_reason() == ESP_RST_SW ? "software"
                                                       : "иное");
    Serial.printf("heap      : %lu Б свободно, %lu Б крупнейший блок\n",
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.printf("frame buf : %lu Б статически в SRAM\n",
                  (unsigned long)sizeof(s_frame));
    Serial.printf("смещения  : MADCTL 0x%02X, CASET +%d, RASET +%d\n",
                  DISP_MADCTL, DISP_OFFSET_X, DISP_OFFSET_Y);
}

void setup()
{
    // Пауза до первого вывода: после сброса хост ещё не открыл порт, и начало
    // лога иначе теряется (буфер HWCDC не бесконечный).
    delay(1500);
    Serial.begin(115200);
    delay(200);

    print_boot_info();

    // Подсветка на 50 %: полная яркость бьёт по глазам на стенде.
    ledcAttach(PIN_DISP_BL, 5000, 8);
    ledcWrite(PIN_DISP_BL, 128);

    pinMode(PIN_DISP_CS, OUTPUT);
    pinMode(PIN_DISP_DC, OUTPUT);
    pinMode(PIN_DISP_RST, OUTPUT);
    digitalWrite(PIN_DISP_CS, HIGH);

    keys_init();

    Serial.println("[disp] инициализация ST7789 на 40 МГц (частота вендора)");
    const int64_t t_init = esp_timer_get_time();
    disp_spi_setup(40 * 1000 * 1000);
    disp_init();
    Serial.printf("[disp] init занял %lu мс\n",
                  (unsigned long)((esp_timer_get_time() - t_init) / 1000));

    // Замеры на трёх частотах. 40 МГц - проверенная вендором точка, 60 и 80 -
    // эксперимент, после которого всё равно возвращаемся на 40.
    static const uint32_t kSpeeds[] = {40, 60, 80};
    for (uint32_t mhz : kSpeeds) {
        const uint32_t hz = mhz * 1000 * 1000;
        if (s_spi_hz != hz) {
            disp_spi_setup(hz);
        }
        // Панель после смены частоты не сбрасываем: состояние команд
        // сохраняется, переинициализация исказила бы замер.
        draw_test_pattern();
        const uint32_t frame_us = disp_send_rows(0, DISP_HEIGHT);
        report_transfer("кадр 240x135", hz, sizeof(s_frame), frame_us);

        // 9 строк - это одна текстовая строка при ячейке 6x9, то есть та
        // самая полоса, которой будет рисовать A1.
        const uint32_t line_us = disp_send_rows(0, 9);
        report_transfer("строка 240x9", hz, DISP_WIDTH * 9 * 2, line_us);
        delay(300);
    }

    // Дать глазам увидеть 80 МГц: картинка та же, но передаётся вдвое быстрее.
    // Артефакты по SPI проявляются как сдвиг или мусор в отдельных строках.
    Serial.println("[глазами] 4 секунды картинка идёт на 80 МГц - смотрите артефакты");
    disp_spi_setup(80 * 1000 * 1000);
    draw_test_pattern();
    disp_send_rows(0, DISP_HEIGHT);
    delay(4000);

    // Рабочая частота - 40 МГц: на ней панель проверена вендором.
    disp_spi_setup(40 * 1000 * 1000);
    draw_test_pattern();
    const uint32_t frame_us = disp_send_rows(0, DISP_HEIGHT);

    Serial.println();
    Serial.println("[глазами] сейчас на экране: 8 вертикальных полос (красный,");
    Serial.println("          зелёный, синий, белый, чёрный, жёлтый, голубой,");
    Serial.println("          малиновый), белая рамка по всему периметру 240x135,");
    Serial.println("          картинка не повёрнута и не зеркальна.");
    report_transfer("рабочий", 40 * 1000 * 1000, sizeof(s_frame), frame_us);
    Serial.println();

    Serial.println("[keys] жду нажатий: каждое печатается как строка/столбец");
    Serial.println();
}

void loop()
{
    const int64_t t0 = esp_timer_get_time();

    static uint64_t prev_keys = 0;
    const uint64_t now_keys = keys_scan();
    if (now_keys != prev_keys) {
        for (int i = 0; i < KEY_LINES * KEY_COLS; ++i) {
            const bool was = prev_keys & (1ULL << i);
            const bool is = now_keys & (1ULL << i);
            if (was != is) {
                Serial.printf("[keys] %s строка %d столбец %d\n",
                              is ? "нажата    " : "отпущена  ", i / KEY_COLS,
                              i % KEY_COLS);
            }
        }
        prev_keys = now_keys;
    }

    // Статус раз в секунду: заодно доказательство, что тик живой и loop
    // не блокируется передачей кадра.
    static int64_t next_status = 0;
    if (t0 >= next_status) {
        next_status = t0 + 1000000;
        const uint32_t batt_mv = analogReadMilliVolts(PIN_BAT_ADC) * 2;
        Serial.printf("[tick] %lu с, heap %lu Б (мин %lu), батарея %lu мВ, G0 %s\n",
                      (unsigned long)(t0 / 1000000),
                      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                      (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                      (unsigned long)batt_mv,
                      digitalRead(PIN_BUTTON_G0) ? "отпущена" : "нажата");
    }

    // Интервал опроса матрицы: 10 мс из docs/board.md, раздел 5.
    const int64_t spent = esp_timer_get_time() - t0;
    const int64_t rest = 10000 - spent;
    if (rest > 0) {
        delayMicroseconds(rest);
    }
}
