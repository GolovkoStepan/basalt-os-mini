// Щуп env:probe: чип, flash, PSRAM и помещается ли кадровый буфер.
//
// Зачем отдельный env, а не строка в env:tdisplay. Проверять это надо ДО
// первой строчки драйвера экрана (docs/board.md, §10, пункт 2). У части партий
// T-Display-S3 PSRAM отсутствует, и тогда весь бюджет памяти
// (docs/FUNCTIONALITY.md, раздел 13) считается заново: кадровый буфер
// 320x170x2 = 108.8 КБ в SRAM рядом с ядром не помещается, и рисовать
// придётся полосой в одну строку, как на Cardputer. Узнать это после того, как
// драйвер написан, - переделывать драйвер.
//
// Щуп ничего не настраивает и не оставляет следов: замерил, напечатал, тикает
// дальше. Тики нужны, чтобы по логу было видно, что система жива, а не зависла
// на замере.
//
// Всё, что здесь измеряется, - вопросы к железу, а не к коду. Поэтому цифры
// попадают в docs/board.md, а не в переписку.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_private/esp_psram_extram.h"
#include "esp_psram.h"
#include "esp_rom_spiflash.h"
#include "sdkconfig.h"

#include "console_usb_jtag.h"
#include "format.h"
#include "indicator.h"
#include "start.h"
#include "tick.h"
#include "vectors.h"

// Кадр экрана: 320x170, 16 бит на пиксель. Ровно то, что понадобится A1.
#define FB_WIDTH   320
#define FB_HEIGHT  170
#define FB_BYTES   ((size_t)FB_WIDTH * (size_t)FB_HEIGHT * 2u)

// Выравнивание буфера, который уйдёт в DMA: адрес и длина кратны размеру
// строки кэша ESP32-S3. Без этого esp_cache_msync не сработает, а картинка
// порвётся полосами - и искать причину в контроллере экрана будет долго.
#define FB_ALIGN   64u

#define MIB        (1024u * 1024u)

// Яркость «жив» на индикаторе. Та же, что в env:bare: задача индикатора -
// «видно с другого конца стола», а не «осветить стол».
#define INDICATOR_ALIVE_LEVEL 40

// ---------------------------------------------------------------------------

static void put_size_kib(size_t bytes)
{
    basalt_put_u64(bytes / 1024u);
    basalt_put(" КБ");
}

// Завести кучу. В проекте её до сих пор не заводил никто.
//
// heap_caps_init() и добавление PSRAM в кучу - это ESP_SYSTEM_INIT_FN с
// приоритетами 100 и 103, а вызывает их do_core_init() из start_cpu0_default,
// то есть функция IDF, которую мы перекрыли своим start_cpu0 (arch/xtensa/
// start.c). A0 это не мешало: там всё на статических буферах. Кадровому
// буферу heap_caps нужен, поэтому щуп делает эти два вызова сам - ровно те же
// и в том же порядке, что IDF.
//
// Место для этого вызова в самой ОС - вопрос вехи A1, а не щупа: сейчас щуп
// отвечает только на вопрос «помещается ли кадр», и отвечать должен честно.
// Пока вызов живёт здесь, любой другой env по-прежнему живёт без кучи, и это
// надо помнить.
static void probe_heap_init(void)
{
    heap_caps_init();

    // Без PSRAM этот вызов вернёт ошибку; на T-Display-S3 её быть не должно,
    // но результат печатаем, а не глотаем: «куча есть» и «куча вместе с PSRAM»
    // - разные утверждения.
    const esp_err_t psram_heap = esp_psram_extram_add_to_heap_allocator();

    basalt_put("[probe] куча: heap_caps_init выполнен, PSRAM в куче ");
    basalt_put(psram_heap == ESP_OK ? "есть" : "НЕТ");
    basalt_put("\n");
}

// Одна строка на область памяти. Три числа, потому что они отвечают на разные
// вопросы: «сколько всего» - бюджет, «сколько свободно» - запас, «крупнейший
// блок» - влезет ли буфер одним куском. Для кадрового буфера важен именно
// третий: при fragmentation 100 КБ свободно, а блока на 108.8 КБ нет.
static void report_heap(const char* name, uint32_t caps)
{
    basalt_put("[probe] ");
    basalt_put(name);

    const size_t total = heap_caps_get_total_size(caps);
    if (total == 0) {
        basalt_put(": нет\n");
        return;
    }

    basalt_put(": всего ");
    put_size_kib(total);
    basalt_put(", свободно ");
    put_size_kib(heap_caps_get_free_size(caps));
    basalt_put(", крупнейший блок ");
    put_size_kib(heap_caps_get_largest_free_block(caps));
    basalt_put("\n");
}

static void put_chip_model(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:
        basalt_put("ESP32");
        break;
    case CHIP_ESP32S2:
        basalt_put("ESP32-S2");
        break;
    case CHIP_ESP32S3:
        basalt_put("ESP32-S3");
        break;
    default:
        // Номер печатаем, а не пишем «неизвестный» молча: по нему номер модели
        // находится в esp_chip_info.h, и это быстрее, чем догадываться.
        basalt_put("модель #");
        basalt_put_u64((uint64_t)model);
        break;
    }
}

// Главный замер: выделить, залить, сверить. Ответ «да» здесь означает, что A1
// может рисовать в полный кадр, а не полосой.
static bool probe_framebuffer(void)
{
    basalt_put("[probe] кадровый буфер ");
    basalt_put_u64(FB_WIDTH);
    basalt_put("x");
    basalt_put_u64(FB_HEIGHT);
    basalt_put("x2 = ");
    basalt_put_u64(FB_BYTES);
    basalt_put(" Б: ");

    void* const fb = heap_caps_aligned_alloc(FB_ALIGN, FB_BYTES, MALLOC_CAP_SPIRAM);
    if (fb == NULL) {
        basalt_put("в PSRAM не выделился\n");
        return false;
    }

    const uintptr_t addr = (uintptr_t)fb;
    basalt_put("адрес 0x");
    basalt_put_hex((uint32_t)addr, 8);
    basalt_put(", выравнивание ");
    basalt_put_u64(FB_ALIGN);
    basalt_put(" Б ");
    basalt_put((addr % FB_ALIGN) == 0u ? "ok" : "НАРУШЕНО");

    const uint64_t fill_start = basalt_uptime_us();
    memset(fb, 0xA5, FB_BYTES);
    const uint64_t fill_end = basalt_uptime_us();

    // Чтение через volatile: иначе компилятор вправе решить, что сравнивает
    // результат memset с константой, и проверка исчезнет вместе с ошибкой.
    const volatile uint8_t* const bytes = (const volatile uint8_t*)fb;
    uint64_t bad = 0;
    for (size_t i = 0; i < FB_BYTES; ++i) {
        if (bytes[i] != 0xA5u) {
            ++bad;
        }
    }
    const uint64_t check_end = basalt_uptime_us();

    basalt_put("\n[probe] заливка ");
    basalt_put_u64(fill_end - fill_start);
    basalt_put(" мкс, сверка ");
    basalt_put_u64(check_end - fill_end);
    basalt_put(" мкс, расхождений ");
    basalt_put_u64(bad);
    basalt_put("\n");

    heap_caps_free(fb);
    return bad == 0;
}

// ---------------------------------------------------------------------------

void basalt_main(void)
{
    // Порядок тот же, что в env:bare: консоль первой, чтобы след остался, если
    // что-то дальше зависнет; пауза до остального вывода, чтобы хост успел
    // открыть порт; векторы - до любой работы с периферией.
    basalt_console_init();
    basalt_tick_init();
    basalt_delay_us(2000000);
    basalt_vectors_init();
    basalt_indicator_init();

    const uint64_t t_boot = basalt_uptime_us();

    basalt_put("\n\nbasalt-os probe\n");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    basalt_put("[probe] чип: ");
    put_chip_model(chip.model);
    basalt_put(", ревизия v");
    basalt_put_u64(chip.revision / 100u);
    basalt_put(".");
    basalt_put_u64(chip.revision % 100u);
    basalt_put(", ядер ");
    basalt_put_u64(chip.cores);
    basalt_put("\n");

    // Размер flash берём из данных ROM, а не из драйвера esp_flash. Драйвер
    // тоже заводится через ESP_SYSTEM_INIT_FN (приоритет 130), которого при
    // нашем старте не было; ради одного числа тянуть его инициализацию с
    // замком и мьютексом смысла нет. Значение заполняет загрузчик, и в его же
    // логе видно «SPI Flash Size : 16MB» - два источника сходятся.
    const uint32_t flash_size = g_rom_flashchip.chip_size;
    if (flash_size != 0) {
        basalt_put("[probe] flash: ");
        basalt_put_u64(flash_size / MIB);
        basalt_put(" МБ (");
        basalt_put_u64(flash_size);
        basalt_put(" Б), по данным ROM\n");
    } else {
        basalt_put("[probe] flash: размер не прочитался\n");
    }

    // PSRAM. Два вопроса подряд, и второй без первого смысла не имеет:
    // esp_psram_get_size() вернёт 0 и когда чипа нет, и когда её не включили в
    // sdkconfig. Различить эти два случая - половина пользы от щупа.
    const bool psram_ok = esp_psram_is_initialized();
    basalt_put("[probe] PSRAM: ");
    if (!psram_ok) {
#ifdef CONFIG_SPIRAM
        basalt_put("не инициализировалась, хотя CONFIG_SPIRAM=y - чипа нет\n");
#else
        basalt_put("нет: CONFIG_SPIRAM в sdkconfig не задан\n");
#endif
    } else {
        basalt_put_u64(esp_psram_get_size() / MIB);
        basalt_put(" МБ (");
        basalt_put_u64(esp_psram_get_size());
        basalt_put(" Б), режим ");
#ifdef CONFIG_SPIRAM_MODE_OCT
        basalt_put("OCT (8 линий)");
#else
        basalt_put("QUAD (4 линии)");
#endif
#ifdef CONFIG_SPIRAM_SPEED
        basalt_put(", ");
        basalt_put_u64(CONFIG_SPIRAM_SPEED);
        basalt_put(" МГц");
#endif
        basalt_put("\n");
    }

    probe_heap_init();

    report_heap("heap SRAM", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    report_heap("heap DMA", MALLOC_CAP_DMA);
    report_heap("heap PSRAM", MALLOC_CAP_SPIRAM);

    const bool fb_ok = probe_framebuffer();

    basalt_put("[probe] вывод: ");
    if (fb_ok) {
        basalt_put("полный кадр в PSRAM помещается, A1 можно строить на кадровом буфере\n");
    } else {
        basalt_put("полного кадра в PSRAM нет - рисовать полосой в одну строку, как на Cardputer\n");
    }

    basalt_put("ready\n\n");

    // Дальше - тот же признак жизни, что у env:bare: номер тика в лог и
    // вспышка индикатора раз в секунду. Без этого нельзя отличить «щуп
    // отработал» от «щуп напечатал половину и завис».
    uint32_t tick = 0;
    uint64_t next = t_boot + 1000000u;
    uint64_t led_off_at = 0;

    for (;;) {
        const uint64_t now = basalt_uptime_us();

        if (now >= next) {
            next += 1000000u;
            ++tick;

            basalt_put("tick ");
            basalt_put_u64(tick);
            basalt_put("  uptime ");
            basalt_put_sec3(now);
            basalt_put(" s\n");

            basalt_indicator_set(0, INDICATOR_ALIVE_LEVEL, 0);
            led_off_at = now + 80000u;
        }

        if (led_off_at != 0 && now >= led_off_at) {
            basalt_indicator_set(0, 0, 0);
            led_off_at = 0;
        }

        basalt_delay_us(1000);
    }
}
