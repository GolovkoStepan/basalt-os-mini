// Щуп env:flash: драйвер встроенного flash, таблица разделов, чтение, стирание,
// запись и сверка.
//
// Зачем отдельный env. Хранилище системы - встроенный flash (решение
// 06.11.2026, docs/ROADMAP.md, веха A9), но при своём старте драйвер flash
// никто не заводит: do_core_init() не выполняется, esp_flash_get_size(NULL)
// возвращает ошибку. Прежде чем писать файловую систему, надо знать, что
// драйвер заводится, что чип отвечает, сколько в нём на самом деле места и
// сколько стоит стирание с записью. Всё это - вопросы к железу, поэтому цифры
// идут в docs/board.md, §10, а не в переписку.
//
// Щуп ничего не настраивает и не оставляет следов: единственный сектор,
// который он стирает и пишет, - первый сектор раздела nvs (`0x9000` в таблице
// `partitions.csv`). NVS в проекте не используется, а в конце сектор
// стирается обратно в 0xFF. Раздел приложения, раздел данных и загрузчик не
// трогаются.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_rom_spiflash.h"

#include "console_usb_jtag.h"
#include "flash.h"
#include "format.h"
#include "indicator.h"
#include "parttable.h"
#include "tick.h"
#include "vectors.h"

// Где таблица разделов: PlatformIO кладёт её на 0x8000
// (CONFIG_PARTITION_TABLE_OFFSET по умолчанию). Сколько читать - 0xC00:
// столько значащих байт в секторе таблицы по формату IDF, остальное забито
// 0xFF. Сам разбор - в storage/parttable.c: он переносимый и проверяется на
// маке, а здесь остаётся только чтение с чипа и печать.
#define PARTITION_TABLE_ADDR 0x8000u
#define PARTITION_TABLE_MAX  0xC00u

// Читаем образ приложения: он точно есть, и читать его безопасно.
#define READ_TEST_ADDR 0x10000u
#define READ_TEST_LEN  (64u * 1024u)

// Яркость «жив» на индикаторе: та же, что у остальных щупов. Задача индикатора -
// «видно с другого конца стола», а не «осветить стол».
#define INDICATOR_ALIVE_LEVEL 40

// ---------------------------------------------------------------------------

static void put_err(int err)
{
    if (err == 0) {
        basalt_put("ok");
        return;
    }
    basalt_put("ошибка 0x");
    basalt_put_hex((uint32_t)err, 4);
}

// Байты в килобайты: в мегабайтах мелкие разделы становятся нулём, а в байтах
// цифру не прочитать глазами.
static void put_kib(uint32_t bytes)
{
    basalt_put_u64(bytes / 1024u);
    basalt_put(" КБ");
}

// Скорость операции. Деление на ноль возможно: быстрая операция может уложиться
// в меньшее, чем один тик таймера, время, и тогда честнее сказать об этом, чем
// напечатать «0 КБ/с».
static void put_speed(uint32_t bytes, uint64_t us)
{
    if (us == 0) {
        basalt_put("меньше микросекунды");
        return;
    }
    basalt_put_u64(((uint64_t)bytes * 1000000u) / (1024u * us));
    basalt_put(" КБ/с");
}

// ---------------------------------------------------------------------------
// Разбор таблицы разделов

// Разбор таблицы: печатает записи и возвращает адрес первого сектора раздела
// nvs (0, если такого раздела нет). Именно nvs, а не первый попавшийся раздел
// данных: после A9 в таблице появится раздел с файлами, и стирать его первым
// сектором щупа нельзя.
static uint32_t report_partitions(void)
{
    uint8_t* const table = heap_caps_malloc(PARTITION_TABLE_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (table == NULL) {
        basalt_put("[flash] разделы: нет памяти под таблицу\n");
        return 0;
    }

    const int err = basalt_flash_read(PARTITION_TABLE_ADDR, table, PARTITION_TABLE_MAX);
    if (err != 0) {
        basalt_put("[flash] разделы: чтение не удалось, ");
        put_err(err);
        basalt_put("\n");
        heap_caps_free(table);
        return 0;
    }

    uint32_t nvs_addr = 0;
    uint32_t end = 0;
    uint32_t count = 0;

    basalt_put("[flash] разделы с 0x");
    basalt_put_hex(PARTITION_TABLE_ADDR, 4);
    basalt_put(":\n");

    for (unsigned i = 0;; ++i) {
        basalt_partition_t part;
        const basalt_part_status_t status =
            basalt_parttable_get(table, PARTITION_TABLE_MAX, i, &part);

        if (status == BASALT_PART_END_MD5) {
            basalt_put("[flash]   дальше контрольная сумма MD5 - конец таблицы\n");
            break;
        }
        if (status != BASALT_PART_OK) {
            // Чужой magic - единственный случай, о котором надо сказать
            // вслух: значит, читали не оттуда или таблицу не заливали.
            // Стёртая flash (END_FF) - обычный конец таблицы, молчим.
            if (status == BASALT_PART_BAD_MAGIC) {
                basalt_put("[flash]   запись ");
                basalt_put_u64(i);
                basalt_put(": чужой magic - на 0x");
                basalt_put_hex(PARTITION_TABLE_ADDR, 8);
                basalt_put(" не таблица разделов\n");
            }
            break;
        }

        basalt_put("[flash]   0x");
        basalt_put_hex(part.address, 6);
        basalt_put("  ");
        put_kib(part.size);
        basalt_put("  тип ");
        basalt_put_u64(part.type);
        basalt_put("/");
        basalt_put_u64(part.subtype);
        basalt_put("  ");
        basalt_put(part.label);
        basalt_put("\n");

        // Тип 1 - данные, подтип 2 - nvs (esp_partition.h,
        // ESP_PARTITION_TYPE_DATA / ESP_PARTITION_SUBTYPE_DATA_NVS).
        if (part.type == 1u && part.subtype == 2u) {
            nvs_addr = part.address;
        }
        if (part.address + part.size > end) {
            end = part.address + part.size;
        }
        ++count;
    }

    if (count == 0) {
        basalt_put("[flash]   записей нет\n");
    } else {
        // Сколько осталось вне разделов - это и есть бюджет под файловую
        // систему. Считаем от конца последнего раздела до физического размера.
        const uint32_t total = basalt_flash_physical_size();
        basalt_put("[flash] всего разделов ");
        basalt_put_u64(count);
        basalt_put(", за последним (0x");
        // Восемь разрядов, а не шесть: конец таблицы равен размеру чипа
        // (0x1000000 у 16 МБ), это семь разрядов, и при шести старший
        // отбрасывался - в логе получалось «за последним (0x000000)».
        basalt_put_hex(end, 8);
        basalt_put(") свободно ");
        if (total > end) {
            put_kib(total - end);
        } else {
            basalt_put("0 КБ - разделы занимают весь чип");
        }
        basalt_put("\n");
    }

    heap_caps_free(table);
    return nvs_addr;
}

// ---------------------------------------------------------------------------
// Замеры

static bool probe_read(void)
{
    uint8_t* const buf = heap_caps_malloc(READ_TEST_LEN, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        basalt_put("[flash] чтение: нет памяти под буфер\n");
        return false;
    }

    const uint64_t t0 = basalt_uptime_us();
    const int err = basalt_flash_read(READ_TEST_ADDR, buf, READ_TEST_LEN);
    const uint64_t t1 = basalt_uptime_us();

    if (err != 0) {
        basalt_put("[flash] чтение ");
        put_kib(READ_TEST_LEN);
        basalt_put(": ");
        put_err(err);
        basalt_put("\n");
        heap_caps_free(buf);
        return false;
    }

    basalt_put("[flash] чтение ");
    put_kib(READ_TEST_LEN);
    basalt_put(" с 0x");
    basalt_put_hex(READ_TEST_ADDR, 6);
    basalt_put(": ");
    basalt_put_u64(t1 - t0);
    basalt_put(" мкс, ");
    put_speed(READ_TEST_LEN, t1 - t0);
    basalt_put("\n");

    heap_caps_free(buf);
    return true;
}

// Стереть сектор, записать узор, сверить, стереть обратно.
//
// Сверка отдельно от записи намеренно: NOR-flash умеет только менять 1 на 0, и
// ошибка «забыли стереть» даёт не отказ, а тихо испорченные байты. Поэтому
// сначала проверяем, что после стирания сектор действительно весь 0xFF.
static bool probe_erase_write(uint32_t sector)
{
    const size_t len = BASALT_FLASH_SECTOR_SIZE;

    uint8_t* const buf = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t* const back = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL || back == NULL) {
        basalt_put("[flash] стирание и запись: нет памяти под буферы\n");
        heap_caps_free(buf);
        heap_caps_free(back);
        return false;
    }

    // Узор не из повторяющегося байта: заливка нулями сошлась бы и при записи
    // одним и тем же словом в неверный адрес.
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (uint8_t)(0x5Au ^ (uint8_t)i);
    }

    basalt_put("[flash] сектор 0x");
    basalt_put_hex(sector, 6);
    basalt_put(": стирание");

    const uint64_t t0 = basalt_uptime_us();
    int err = basalt_flash_erase_sector(sector);
    const uint64_t t1 = basalt_uptime_us();

    if (err != 0) {
        basalt_put(" - ");
        put_err(err);
        basalt_put("\n");
        heap_caps_free(buf);
        heap_caps_free(back);
        return false;
    }
    basalt_put(" ");
    basalt_put_u64(t1 - t0);
    basalt_put(" мкс");

    // Проверка стирания: весь сектор должен читаться как 0xFF.
    err = basalt_flash_read(sector, back, len);
    if (err != 0) {
        basalt_put(", чтение после стирания - ");
        put_err(err);
        basalt_put("\n");
        heap_caps_free(buf);
        heap_caps_free(back);
        return false;
    }
    uint32_t dirty = 0;
    for (size_t i = 0; i < len; ++i) {
        if (back[i] != 0xFFu) {
            ++dirty;
        }
    }
    basalt_put(", не 0xFF байт ");
    basalt_put_u64(dirty);
    basalt_put("\n");

    const uint64_t t2 = basalt_uptime_us();
    err = basalt_flash_write(sector, buf, len);
    const uint64_t t3 = basalt_uptime_us();

    if (err != 0) {
        basalt_put("[flash] запись ");
        put_kib((uint32_t)len);
        basalt_put(" - ");
        put_err(err);
        basalt_put("\n");
        heap_caps_free(buf);
        heap_caps_free(back);
        return false;
    }

    basalt_put("[flash] запись ");
    put_kib((uint32_t)len);
    basalt_put(": ");
    basalt_put_u64(t3 - t2);
    basalt_put(" мкс, ");
    put_speed((uint32_t)len, t3 - t2);
    basalt_put("\n");

    err = basalt_flash_read(sector, back, len);
    uint32_t bad = 0;
    if (err != 0) {
        basalt_put("[flash] сверка: чтение - ");
        put_err(err);
        basalt_put("\n");
    } else {
        for (size_t i = 0; i < len; ++i) {
            if (back[i] != buf[i]) {
                ++bad;
            }
        }
        basalt_put("[flash] сверка: расхождений ");
        basalt_put_u64(bad);
        basalt_put("\n");
    }

    // Возвращаем сектор в исходное состояние: щуп не должен оставлять мусор,
    // иначе следующий замер начнёт не с чистого листа.
    err = basalt_flash_erase_sector(sector);
    basalt_put("[flash] сектор 0x");
    basalt_put_hex(sector, 6);
    basalt_put(" стёрт обратно: ");
    put_err(err);
    basalt_put("\n");

    heap_caps_free(buf);
    heap_caps_free(back);
    return bad == 0 && dirty == 0;
}

// ---------------------------------------------------------------------------

static void report_driver(void)
{
    const int err = basalt_flash_init();

    basalt_put("[flash] драйвер: ");
    if (err != 0) {
        basalt_put("не завёлся, ");
        put_err(err);
        basalt_put(" - без него дальше некуда\n");
        return;
    }
    if (basalt_flash_id() != 0) {
        basalt_put("заведён, JEDEC ID 0x");
        basalt_put_hex(basalt_flash_id(), 6);
    } else {
        basalt_put("заведён, JEDEC ID не прочитался");
    }
    basalt_put("\n");

    const uint32_t declared = basalt_flash_size();
    const uint32_t physical = basalt_flash_physical_size();

    basalt_put("[flash] размер: объявлен ");
    put_kib(declared);
    basalt_put(", физический ");
    put_kib(physical);
    basalt_put("\n");

    // Расхождение - не мелочь: драйвер отказывается читать и писать за
    // объявленной границей, то есть место физически есть, а использовать его
    // нельзя. Объявленный размер приходит из g_rom_flashchip, который заполняет
    // загрузчик; если он когда-нибудь окажется меньше физического, увидеть это
    // надо здесь, а не посреди файловой системы.
    basalt_put("[flash] вывод: ");
    if (physical > declared) {
        basalt_put("объявлено меньше, чем есть; лечится размером flash в sdkconfig и своей таблицей разделов (A9)\n");
    } else if (physical == 0) {
        basalt_put("физический размер чип не сообщил; верить объявленному\n");
    } else {
        basalt_put("объявленный размер совпадает с физическим\n");
    }

    // Сектор, блок и страницу берём из данных ROM: это то, чем чип себя
    // описал загрузчику, а не наши предположения.
    basalt_put("[flash] ROM: сектор ");
    put_kib(g_rom_flashchip.sector_size);
    basalt_put(", блок ");
    put_kib(g_rom_flashchip.block_size);
    basalt_put(", страница ");
    basalt_put_u64(g_rom_flashchip.page_size);
    basalt_put(" Б\n");
}

void basalt_main(void)
{
    // Порядок тот же, что в env:probe: консоль первой, чтобы след остался, если
    // что-то дальше зависнет; пауза до остального вывода, чтобы хост успел
    // открыть порт; векторы - до любой работы с периферией.
    basalt_console_init();
    basalt_tick_init();
    basalt_delay_us(2000000);
    basalt_vectors_init();
    basalt_indicator_init();

    const uint64_t t_boot = basalt_uptime_us();

    basalt_put("\n\nbasalt-os flash probe\n");

    report_driver();

    if (!basalt_flash_ready()) {
        basalt_put("ready\n\n");
    } else {
        const uint32_t nvs = report_partitions();

        const bool read_ok = probe_read();

        // Без раздела nvs стирать нечего: стирать наугад нельзя, а первый
        // сектор раздела приложения - это загрузочный образ.
        bool write_ok = false;
        if (nvs == 0) {
            basalt_put("[flash] раздела nvs в таблице нет - стирание и запись не проверялись\n");
        } else if (nvs % BASALT_FLASH_SECTOR_SIZE != 0u) {
            basalt_put("[flash] адрес nvs не выровнен по сектору - стирание не проверялось\n");
        } else {
            write_ok = probe_erase_write(nvs);
        }

        basalt_put("[flash] свободно в SRAM: ");
        put_kib((uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        basalt_put("\n");

        basalt_put("[flash] вывод: ");
        if (read_ok && write_ok) {
            basalt_put("драйвер рабочий: чтение, стирание, запись и сверка сошлись, A9 можно строить на нём\n");
        } else {
            basalt_put("драйвер не подтверждён - смотреть строки выше\n");
        }

        basalt_put("ready\n\n");
    }

    // Дальше - тот же признак жизни, что у остальных щупов: без него нельзя
    // отличить «щуп отработал» от «щуп напечатал половину и завис на стирании».
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
