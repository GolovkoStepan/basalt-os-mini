// Щуп env:fs: своя ФС BFS на разделе storage.
//
// Зачем отдельный env. Тесты на маке проверяют формат, карту блоков и
// переживание обрыва питания, но ничего не говорят о железе: сколько стоит
// форматирование раздела на 14 МБ, сколько - монтирование, держится ли
// запись на настоящем NOR-flash и совпадает ли прочитанное с записанным.
// Всё это вопросы к железу, поэтому цифры идут в docs/board.md, §10.
//
// Щуп пишет в раздел storage - тот, что отдан файловой системе, - и размечает
// его заново, если он ещё не размечен. Данных пользователя там пока нет, а
// после первой разметки повторный запуск только монтирует и дописывает файлы;
// поэтому прогон дважды - это ещё и проверка, что ФС читается после
// перезагрузки, а не только сразу после записи.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "bfs.h"
#include "bfs_flash.h"
#include "console_usb_jtag.h"
#include "flash.h"
#include "format.h"
#include "indicator.h"
#include "tick.h"
#include "vectors.h"

#define INDICATOR_ALIVE_LEVEL 40

// Размер проверочного файла. 64 КБ - это 16 блоков: хватает, чтобы увидеть и
// переход через границу блока, и цену записи, и не растянуть прогон на минуту.
#define BIG_FILE_SIZE (64u * 1024u)

// ---------------------------------------------------------------------------
// Печать
// ---------------------------------------------------------------------------

static void put_err(int err)
{
    if (err == 0) {
        basalt_put("ok");
        return;
    }
    // Для esp_err_t печатаем шестнадцатеричный код, для кодов BFS - число:
    // коды BFS маленькие и отрицательные, в шестнадцатеричном виде их не
    // прочитать.
    if (err < 0) {
        basalt_put("код ");
        basalt_put_u64((uint64_t)(-err));
        basalt_put(" (");
        switch (err) {
        case BFS_ERR_IO: basalt_put("отказ устройства"); break;
        case BFS_ERR_FORMAT: basalt_put("не BFS или суперблок повреждён"); break;
        case BFS_ERR_NO_SPACE: basalt_put("нет места"); break;
        case BFS_ERR_NOT_FOUND: basalt_put("не найдено"); break;
        case BFS_ERR_EXISTS: basalt_put("уже есть"); break;
        case BFS_ERR_NOT_DIR: basalt_put("не каталог"); break;
        case BFS_ERR_IS_DIR: basalt_put("это каталог"); break;
        case BFS_ERR_NOT_EMPTY: basalt_put("каталог не пуст"); break;
        case BFS_ERR_NAME: basalt_put("негодное имя"); break;
        case BFS_ERR_ARG: basalt_put("негодный аргумент"); break;
        case BFS_ERR_BROKEN: basalt_put("ФС сломана, нужен remount"); break;
        case BASALT_FS_ERR_NO_FLASH: basalt_put("драйвер flash не завёлся"); break;
        case BASALT_FS_ERR_NO_PARTITION: basalt_put("в таблице разделов нет раздела с такой меткой"); break;
        case BASALT_FS_ERR_BAD_PARTITION: basalt_put("раздел негоден: не выровнен или не того размера"); break;
        default: basalt_put("неизвестный код"); break;
        }
        basalt_put(")");
        return;
    }
    basalt_put("ошибка 0x");
    basalt_put_hex((uint32_t)err, 4);
}

static void put_kib(uint32_t bytes)
{
    basalt_put_u64(bytes / 1024u);
    basalt_put(" КБ");
}

static void put_ms(uint64_t us)
{
    // Миллисекунды с десятыми: стирание сектора - 45 мс, и целые миллисекунды
    // уже дают полезную цифру, а микросекунды в длинных операциях не читаются.
    basalt_put_u64(us / 1000u);
    basalt_put(".");
    basalt_put_u64((us % 1000u) / 100u);
    basalt_put(" мс");
}

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
// Блочное устройство и раздел
//
// Сама привязка (поиск раздела storage, проверка границ, адаптер поверх
// драйвера flash) живёт в storage/bfs_flash.c - её же зовёт оболочка при
// старте. Здесь только измерения: щуп не должен уметь то, чего не умеет
// система, иначе «в щупе работает» перестанет что-то значить.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Данные для проверки
// ---------------------------------------------------------------------------

// Узор не из повторяющегося байта: заливка нулями сошлась бы и при записи
// одним и тем же словом в неверный адрес.
static uint8_t pattern_byte(uint32_t offset)
{
    return (uint8_t)(0x5Au ^ (uint8_t)(offset * 31u));
}

// ---------------------------------------------------------------------------

static bfs_t s_fs;

// Монтирование, а если раздел не размечен - разметка. Возвращает 0 или код.
//
// Меряем отдельно монтирование, разметку и монтирование после неё: у щупа
// ценность ровно в этих числах, а не в одной сумме. Поэтому здесь два вызова
// basalt_fs_mount (сначала без разметки), а не один с format_if_empty.
static int mount_or_format(uint32_t part_size)
{
    uint64_t t0 = basalt_uptime_us();
    int rc = basalt_fs_mount(&s_fs, "storage", false, NULL);
    uint64_t t1 = basalt_uptime_us();

    basalt_put("[fs] монтирование: ");
    if (rc == 0) {
        put_ms(t1 - t0);
        basalt_put("\n");
        return 0;
    }
    if (rc != BFS_ERR_FORMAT) {
        basalt_put("не удалось, ");
        put_err(rc);
        basalt_put("\n");
        return rc;
    }

    basalt_put("раздел не размечен - размечаю\n");

    t0 = basalt_uptime_us();
    rc = basalt_fs_format(&s_fs, "storage");
    t1 = basalt_uptime_us();
    if (rc != 0) {
        basalt_put("[fs] форматирование: не удалось, ");
        put_err(rc);
        basalt_put("\n");
        return rc;
    }
    basalt_put("[fs] форматирование ");
    put_kib(part_size);
    basalt_put(": ");
    put_ms(t1 - t0);
    basalt_put("\n");

    // Монтирование после разметки - не формальность: format оставил ФС в
    // памяти, а дальше всё должно читаться с диска, как после перезагрузки.
    t0 = basalt_uptime_us();
    rc = basalt_fs_mount(&s_fs, "storage", false, NULL);
    t1 = basalt_uptime_us();
    basalt_put("[fs] монтирование после разметки: ");
    if (rc != 0) {
        put_err(rc);
        basalt_put("\n");
        return rc;
    }
    put_ms(t1 - t0);
    basalt_put("\n");
    return 0;
}

static bool write_files(void)
{
    uint8_t* const big = heap_caps_malloc(BIG_FILE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (big == NULL) {
        basalt_put("[fs] запись: нет памяти под буфер (нужен SRAM)\n");
        return false;
    }

    bool ok = true;
    int rc = bfs_mkdir(&s_fs, "/etc");
    if (rc == BFS_ERR_EXISTS) {
        rc = BFS_OK;
    }
    basalt_put("[fs] mkdir /etc: ");
    put_err(rc);
    basalt_put("\n");
    ok = ok && rc == BFS_OK;

    if (ok) {
        // Текст с кириллицей: путь проверки UTF-8 насквозь, от .rodata до
        // обратного чтения.
        const char rc_text[] = "echo привет\n";
        rc = bfs_create(&s_fs, "/etc/rc");
        if (rc == BFS_ERR_EXISTS) {
            rc = BFS_OK;
        }
        uint32_t done = 0;
        if (rc == BFS_OK) {
            rc = bfs_write(&s_fs, "/etc/rc", 0, rc_text, sizeof(rc_text) - 1u, &done);
        }
        basalt_put("[fs] /etc/rc (");
        basalt_put_u64(sizeof(rc_text) - 1u);
        basalt_put(" Б): ");
        put_err(rc);
        basalt_put("\n");
        ok = ok && rc == BFS_OK && done == sizeof(rc_text) - 1u;
    }

    for (uint32_t i = 0; i < BIG_FILE_SIZE; ++i) {
        big[i] = pattern_byte(i);
    }

    if (ok) {
        rc = bfs_create(&s_fs, "/big");
        if (rc == BFS_ERR_EXISTS) {
            rc = BFS_OK;
        }
        if (rc != BFS_OK) {
            basalt_put("[fs] создание /big: ");
            put_err(rc);
            basalt_put("\n");
            ok = false;
        }
    }

    if (ok) {
        const uint64_t t0 = basalt_uptime_us();
        uint32_t done = 0;
        rc = bfs_write(&s_fs, "/big", 0, big, BIG_FILE_SIZE, &done);
        const uint64_t t1 = basalt_uptime_us();

        basalt_put("[fs] запись /big ");
        put_kib(BIG_FILE_SIZE);
        basalt_put(": ");
        if (rc != BFS_OK) {
            put_err(rc);
        } else {
            put_ms(t1 - t0);
            basalt_put(", ");
            put_speed(BIG_FILE_SIZE, t1 - t0);
            if (done != BIG_FILE_SIZE) {
                basalt_put(" - записано ");
                basalt_put_u64(done);
                basalt_put(" Б");
            }
        }
        basalt_put("\n");
        ok = ok && rc == BFS_OK && done == BIG_FILE_SIZE;
    }

    // Чтение сразу после записи: страница кэша чипа тут ни при чём (драйвер
    // читает мимо кэша), поэтому расхождение означало бы ошибку в карте блоков.
    if (ok) {
        uint32_t done = 0;
        rc = bfs_read(&s_fs, "/big", 0, big, BIG_FILE_SIZE, &done);
        uint32_t bad = 0;
        if (rc == BFS_OK) {
            for (uint32_t i = 0; i < BIG_FILE_SIZE; ++i) {
                if (big[i] != pattern_byte(i)) {
                    ++bad;
                }
            }
        }
        basalt_put("[fs] сверка /big: ");
        if (rc != BFS_OK) {
            put_err(rc);
        } else {
            basalt_put("расхождений ");
            basalt_put_u64(bad);
        }
        basalt_put("\n");
        ok = ok && rc == BFS_OK && bad == 0;
    }

    heap_caps_free(big);
    return ok;
}

// Чтение после перемонтирования: то, что лежит на диске, а не в памяти.
static bool verify_after_remount(void)
{
    const uint64_t t0 = basalt_uptime_us();
    const int rc = basalt_fs_mount(&s_fs, "storage", false, NULL);
    const uint64_t t1 = basalt_uptime_us();

    basalt_put("[fs] перемонтирование: ");
    if (rc != 0) {
        put_err(rc);
        basalt_put("\n");
        return false;
    }
    put_ms(t1 - t0);
    basalt_put("\n");

    bool ok = true;

    uint32_t size = 0;
    uint16_t type = 0;
    int err = bfs_stat(&s_fs, "/etc/rc", &size, &type);
    basalt_put("[fs] /etc/rc: размер ");
    if (err != BFS_OK) {
        put_err(err);
        ok = false;
    } else {
        basalt_put_u64(size);
        basalt_put(" Б, тип ");
        basalt_put_u64(type == BFS_TYPE_DIR ? 1u : 0u);
        basalt_put(type == BFS_TYPE_DIR ? " (каталог)" : " (файл)");
        if (type != BFS_TYPE_FILE || size != sizeof("echo привет\n") - 1u) {
            basalt_put(" - не то, что писали");
            ok = false;
        }
    }
    basalt_put("\n");

    uint8_t* const big = heap_caps_malloc(BIG_FILE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (big == NULL) {
        basalt_put("[fs] сверка после перемонтирования: нет памяти\n");
        return false;
    }

    uint32_t done = 0;
    const uint64_t t_read0 = basalt_uptime_us();
    err = bfs_read(&s_fs, "/big", 0, big, BIG_FILE_SIZE, &done);
    const uint64_t t_read1 = basalt_uptime_us();
    uint32_t bad = 0;
    if (err == BFS_OK) {
        for (uint32_t i = 0; i < BIG_FILE_SIZE; ++i) {
            if (big[i] != pattern_byte(i)) {
                ++bad;
            }
        }
    }
    basalt_put("[fs] /big после перемонтирования: ");
    if (err != BFS_OK) {
        put_err(err);
        ok = false;
    } else if (done != BIG_FILE_SIZE || bad != 0) {
        basalt_put("прочитано ");
        basalt_put_u64(done);
        basalt_put(" Б, расхождений ");
        basalt_put_u64(bad);
        ok = false;
    } else {
        basalt_put("сошлось за ");
        put_ms(t_read1 - t_read0);
        basalt_put(", ");
        put_speed(BIG_FILE_SIZE, t_read1 - t_read0);
    }
    basalt_put("\n");
    heap_caps_free(big);

    // Обход корня: имена должны быть те, что писали, а не мусор из надгробий.
    basalt_put("[fs] /:");
    char name[BFS_NAME_MAX + 1];
    bool saw_etc = false;
    for (uint32_t i = 0;; ++i) {
        uint16_t child_type = 0;
        uint32_t child_size = 0;
        err = bfs_readdir(&s_fs, "/", i, name, &child_type, &child_size);
        if (err == BFS_ERR_NOT_FOUND) {
            break;
        }
        if (err != BFS_OK) {
            basalt_put(" обход не удался, ");
            put_err(err);
            ok = false;
            break;
        }
        if (strcmp(name, "etc") == 0) {
            saw_etc = true;
        }
        basalt_put(" ");
        basalt_put(name);
        if (child_type == BFS_TYPE_DIR) {
            basalt_put("/");
        } else {
            basalt_put("(");
            basalt_put_u64(child_size);
            basalt_put(")");
        }
    }
    basalt_put("\n");
    if (!saw_etc) {
        basalt_put("[fs] в корне нет etc - обход читает не то\n");
        ok = false;
    }

    uint32_t used = 0;
    uint32_t total = 0;
    err = bfs_usage(&s_fs, &used, &total);
    basalt_put("[fs] занято блоков: ");
    if (err != BFS_OK) {
        put_err(err);
        ok = false;
    } else {
        basalt_put_u64(used);
        basalt_put(" из ");
        basalt_put_u64(total);
        basalt_put(", свободно ");
        put_kib((total - used) * BFS_BLOCK_SIZE);
    }
    basalt_put("\n");

    return ok;
}

void basalt_main(void)
{
    // Порядок тот же, что у остальных щупов: консоль первой, пауза, чтобы хост
    // успел открыть порт, векторы - до работы с периферией.
    basalt_console_init();
    basalt_tick_init();
    basalt_delay_us(2000000);
    basalt_vectors_init();
    basalt_indicator_init();

    const uint64_t t_boot = basalt_uptime_us();

    basalt_put("\n\nbasalt-os bfs probe\n");

    bool ok = false;

    const int flash_err = basalt_flash_init();
    if (flash_err != 0) {
        basalt_put("[fs] драйвер flash не завёлся, ");
        put_err(flash_err);
        basalt_put("\n");
    } else {
        basalt_put("[fs] драйвер flash: заведён, размер ");
        put_kib(basalt_flash_size());
        basalt_put("\n");

        uint32_t address = 0;
        uint32_t size = 0;
        const int part_rc = basalt_fs_partition("storage", &address, &size);
        if (part_rc != 0) {
            basalt_put("[fs] раздел storage: ");
            put_err(part_rc);
            basalt_put("\n");
        } else {
            basalt_put("[fs] раздел storage: 0x");
            basalt_put_hex(address, 6);
            basalt_put(", ");
            put_kib(size);
            basalt_put(", блоков ");
            basalt_put_u64(size / BFS_BLOCK_SIZE);
            basalt_put("\n");

            // Выравнивание и размер раздела проверяет basalt_fs_mount: две
            // проверки на одно условие однажды разойдутся, и щуп начнёт
            // пропускать то, на чём система откажется работать.
            if (mount_or_format(size) == 0) {
                ok = write_files();
                ok = verify_after_remount() && ok;
            }
        }
    }

    basalt_put("[fs] свободно в SRAM: ");
    put_kib((uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    basalt_put("\n");

    basalt_put("[fs] вывод: ");
    if (ok) {
        basalt_put("BFS на железе работает: разметка, запись, чтение, сверка и перемонтирование сошлись\n");
    } else {
        basalt_put("BFS не подтверждена - смотреть строки выше\n");
    }

    basalt_put("ready\n\n");

    // Признак жизни: без него не отличить «щуп отработал» от «напечатал
    // половину и завис на стирании».
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
