// WS2812 на G21 через RMT - по регистрам, без esp_driver_rmt.
//
// Как устроен кадр. WS2812 принимает 24 бита (по 8 на зелёный, красный, синий,
// старшим битом вперёд), и каждый бит кодируется длиной высокого уровня:
// короткий импульс - 0, длинный - 1. Пауза между кадрами - не меньше 50 мкс
// низкого уровня, по ней светодиод понимает, что кадр кончился.
//
// Что делает RMT. Память канала - 48 слов; каждое слово описывает два
// отрезка: «столько тиков такой уровень, потом столько тиков другой».
// Мы заполняем 24 слова, ставим бит TX_START - и дальше железо само
// выщёлкивает уровни с точностью до тика, не отвлекаясь на прерывания.
//
// Где здесь грабли (все три проверены на плате и все три молчат):
//  - доступ к памяти вместо FIFO (APB_FIFO_MASK);
//  - два делителя частоты, рабочее сочетание одно;
//  - нулевое слово обрывает кадр, поэтому хвост памяти обнуляется;
//  - порядок байт у WS2812 не RGB, а GRB;
//  - без сброса указателя чтения второй кадр начнётся с середины памяти.

#include "led_ws2812.h"

#include "board_pins.h"
#include "tick.h"

#include "soc/gpio_reg.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/rmt_reg.h"
#include "soc/system_reg.h"

// Память каналов RMT лежит отдельным окном в 0x800 от базы блока: по 48 слов
// на канал, первыми идут передающие. Канал 0 - первое слово окна.
#define RMT_MEM_BASE (DR_REG_RMT_BASE + 0x800u)

// Разрешение. У RMT два делителя: общий на группу каналов и отдельный на
// канал. Берём схему IDF - общий 1, канальный 8, итого 80 МГц / 8 = 10 МГц,
// то есть 1 тик = 100 нс.
//
// Проверено на плате: при обратном сочетании (общий 8, канальный 1) кадры не
// принимались вообще, при этом передатчик честно рапортовал об успехе.
// Предположительно частота бита берётся от канального делителя, и вместо
// 10 МГц получалось 80 МГц - такой сигнал WS2812 не разбирает.
#define LED_GROUP_DIV 1u
#define LED_CHANNEL_DIV 8u

// Длительности из даташита WS2812B: T0H 0.4+-0.15 мкс, T1H 0.8+-0.15 мкс,
// период 1.25+-0.6 мкс. Берём 3 / 9 тиков при периоде 12 - все три числа
// внутри допуска, а период ровный, без накопления ошибки на 24 битах.
// Те же значения использует заводская библиотека M5Unified.
#define LED_TICKS_PER_BIT 12
#define LED_T0H_TICKS 3

// Сколько ждать после окончания кадра. WS2812 различает кадры по паузе:
// по даташиту не меньше 50 мкс, у WS2812B-V5 - 280 мкс. Берём 300 мкс, как
// в заводской библиотеке M5Unified, чтобы не зависеть от ревизии светодиода.
#define LED_RESET_US 300u

// Блок памяти канала - 48 слов, а кадр занимает 24. Хвост обязательно
// обнуляется: передатчик идёт по блоку, пока не встретит слово с двумя
// нулевыми длительностями, а незаписанные слова были бы мусором в линии.
#define LED_MEM_WORDS 48u
#define LED_FRAME_WORDS 24u

// Результат проверки памяти, см. basalt_led_memory_ok().
static bool mem_writable;

// Инициализирован ли драйвер. Нужен панике: она хочет зажечь красный, но
// до инициализации трогать RMT нельзя - он ещё не тактируется.
static bool ready;

// Длительность последнего кадра в микросекундах, см. basalt_led_frame_us().
static uint32_t last_frame_us;

// Пин как обычный выход, поднятый в единицу.
//
// Зачем отдельная функция: у ESP32-S3 пины 0..31 и 32..48 живут в разных
// наборах регистров, и одним сдвигом на 38 бит нужный бит не адресовать -
// компилятор справедливо ругается на переполнение сдвига.
static void pin_output_high(unsigned pin)
{
    const uint32_t bit = 1u << (pin & 31u);
    if (pin < 32) {
        SET_PERI_REG_MASK(GPIO_ENABLE_W1TS_REG, bit);
        SET_PERI_REG_MASK(GPIO_OUT_W1TS_REG, bit);
    } else {
        SET_PERI_REG_MASK(GPIO_ENABLE1_W1TS_REG, bit);
        SET_PERI_REG_MASK(GPIO_OUT1_W1TS_REG, bit);
    }
}

// Переключить IO_MUX на функцию GPIO.
//
// Нужно потому, что пин по умолчанию может быть отдан другой периферии, и
// тогда сигнал с матрицы GPIO до ножки не доходит вообще. В Arduino это
// делал pinMode внутри себя, здесь приходится явно.
static void pin_as_gpio(unsigned pin)
{
    if (pin < SOC_GPIO_PIN_COUNT) {
        const uint32_t reg = GPIO_PIN_MUX_REG[pin];
        if (reg != 0) {
            REG_SET_FIELD(reg, MCU_SEL, PIN_FUNC_GPIO);
        }
    }
}

// Проверка «память канала вообще доступна на запись».
//
// Нужна потому, что при неверном режиме доступа записи по адресу памяти
// молча уходят в APB FIFO: канал передаёт нули, светодиод не реагирует, а в логе
// всё выглядит успешно. Стоило одного круга отладки - пусть будет видно сразу.
static bool probe_memory(void)
{
    volatile uint32_t* mem = (volatile uint32_t*)RMT_MEM_BASE;
    mem[0] = 0x5A5A5A5Au;
    const bool ok = (mem[0] == 0x5A5A5A5Au);
    mem[0] = 0;
    return ok;
}

static void send_grb(uint8_t g, uint8_t r, uint8_t b)
{
    volatile uint32_t* mem = (volatile uint32_t*)RMT_MEM_BASE;

    // Порядок байт у WS2812 - G, R, B. Перепутанный порядок выглядит как
    // «просили красный, светится зелёный», и искать это в таймингах - долго.
    const uint8_t bytes[3] = {g, r, b};

    uint32_t word = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const uint32_t one = (bytes[i] >> bit) & 1u;
            const uint32_t high = one ? (LED_TICKS_PER_BIT - LED_T0H_TICKS)
                                      : LED_T0H_TICKS;
            const uint32_t low = LED_TICKS_PER_BIT - high;

            // Слово символа: длительность 0, уровень 0, длительность 1,
            // уровень 1. Уровень 1 у второго отрезка = 0 (линия вниз).
            mem[word++] = high | (1u << 15) | (low << 16);
        }
    }

    // Хвост блока - нули. Символ нулевой длины не выдаёт уровней, поэтому
    // цвет заканчивается ровно на 24-м слове, а не тянет за собой мусор.
    while (word < LED_MEM_WORDS) {
        mem[word++] = 0;
    }

    // Сброс указателя чтения. Без него второй кадр пойдёт с того места, где
    // кончился первый, то есть с середины блока.
    SET_PERI_REG_MASK(RMT_CH0CONF0_REG, RMT_MEM_RD_RST_CH0);
    CLEAR_PERI_REG_MASK(RMT_CH0CONF0_REG, RMT_MEM_RD_RST_CH0);

    // CONF_UPDATE - «перенести записанные настройки в работающее железо»:
    // без него изменения регистров не доходят до автомата передачи.
    SET_PERI_REG_MASK(RMT_CH0CONF0_REG, RMT_CONF_UPDATE_CH0);

    // Замер длительности кадра. 24 бита по 1.2 мкс - это ~29 мкс; если
    // вышло в восемь раз меньше, делитель частоты стоит неверно, и кадр
    // физически не может быть принят светодиодом.
    const uint64_t started = basalt_uptime_us();
    SET_PERI_REG_MASK(RMT_CH0CONF0_REG, RMT_TX_START_CH0);

    // Ждём, пока вычитается весь кадр: адрес чтения должен дойти до нулевого
    // слова-терминатора, на котором передатчик останавливается. Ждать
    // «состояние = простой» нельзя - сразу после старта автомат ещё в простое,
    // и замер выходит нулевым.
    for (uint32_t spin = 0; spin < 100000; ++spin) {
        if ((READ_PERI_REG(RMT_CH0STATUS_REG) & RMT_MEM_RADDR_EX_CH0_M) >
            LED_FRAME_WORDS) {
            break;
        }
    }
    last_frame_us = (uint32_t)(basalt_uptime_us() - started);

    // Пауза между кадрами: без неё светодиод склеит соседние кадры в один.
    basalt_delay_us(LED_RESET_US);
}

void basalt_led_init(void)
{
    // Питание светодиода. На Cardputer оно висит на линии подсветки G38, и
    // поднимаем её явно: без этого светодиод может не загореться вовсе,
    // независимо от того, что приходит по данным. Побочный эффект - вместе
    // со светодиодом включается подсветка экрана.
    pin_output_high(PIN_DISP_BL);

    // Сброс и тактирование блока. Сброс нужен, чтобы не зависеть от того,
    // в каком состоянии RMT оставил загрузчик.
    SET_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_RMT_RST);
    CLEAR_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_RMT_RST);
    SET_PERI_REG_MASK(SYSTEM_PERIP_CLK_EN0_REG, SYSTEM_RMT_CLK_EN);

    // Источник тактирования - APB, общий делитель группы 1, канальный - ниже.
    //
    // APB_FIFO_MASK = 1 - это, вопреки названию, «доступ к памяти, а не к
    // FIFO». Без него записи по адресу RMTMEM уходят в FIFO, канал читает
    // пустую память и передаёт нули: светодиод молчит, ошибок нет.
    WRITE_PERI_REG(RMT_SYS_CONF_REG,
                   RMT_APB_FIFO_MASK |
                   (1u << RMT_SCLK_SEL_S) |
                   ((LED_GROUP_DIV - 1u) << RMT_SCLK_DIV_NUM_S) |
                   RMT_SCLK_ACTIVE);

    // Канал 0: канальный делитель 8, один блок памяти, в простое линия
    // прижата к нулю и выход при этом активен. Прижатый ноль важен: иначе
    // пин в воздухе, и вход собирает шум.
    WRITE_PERI_REG(RMT_CH0CONF0_REG,
                   ((LED_CHANNEL_DIV - 1u) << RMT_DIV_CNT_CH0_S) |
                   (1u << RMT_MEM_SIZE_CH0_S) |
                   RMT_IDLE_OUT_EN_CH0);

    // Маршрутизация: выход канала 0 -> пин G21.
    // OEN_SEL = 0 - выход включён всегда (через GPIO_ENABLE), а не по
    // разрешению от периферии. Иначе в паузах между кадрами пин оказался бы
    // отключён, и «прижатый ноль» из настроек канала ни на что бы не влиял.
    pin_as_gpio(PIN_RGB_LED_G21);
    pin_as_gpio(PIN_DISP_BL);
    SET_PERI_REG_MASK(GPIO_ENABLE_W1TS_REG, 1u << PIN_RGB_LED_G21);
    WRITE_PERI_REG(GPIO_FUNC0_OUT_SEL_CFG_REG + 4u * PIN_RGB_LED_G21,
                   RMT_SIG_OUT0_IDX);

    mem_writable = probe_memory();

    // Погасить: на плате до сброса мог остаться цвет от прошлой прошивки,
    // WS2812 его защёлкивает и держит, пока не придёт новый кадр.
    basalt_led_set(0, 0, 0);

    ready = true;
}

bool basalt_led_ready(void)
{
    return ready;
}

bool basalt_led_memory_ok(void)
{
    return mem_writable;
}

uint32_t basalt_led_frame_us(void)
{
    return last_frame_us;
}

void basalt_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    send_grb(g, r, b);
}
