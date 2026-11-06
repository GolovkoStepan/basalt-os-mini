// Встроенный flash: заведение драйвера и пять операций поверх него. См. flash.h.

#include "flash.h"

#include "esp_err.h"
#include "esp_flash.h"
#include "esp_private/esp_flash_internal.h"

// Состояние в статике, а не в структуре у вызывающего: flash на плате один, и
// второй экземпляр драйвера означал бы два независимых представления о том,
// что уже стёрто. Пока это так, интерфейс может быть без аргумента-дескриптора.
static bool s_ready;
static uint32_t s_size;
static uint32_t s_physical_size;
static uint32_t s_id;

int basalt_flash_init(void)
{
    if (s_ready) {
        return 0;
    }

    // Заводим чип без OS-функций: esp_flash_noos_functions гасят кэш на время
    // операции и ничего не требуют от системы. esp_flash_app_init() здесь был
    // бы не просто лишним, а вредным: он создаёт рекурсивный мьютекс
    // FreeRTOS и замки mmap, а планировщика у нас нет.
    //
    // Функция в приватном заголовке IDF, как и добавление PSRAM в кучу в
    // heap.c: публичного «заведи драйвер flash» в IDF нет, потому что в
    // штатной прошивке это делает do_core_init().
    const int err = esp_flash_init_default_chip();
    if (err != 0) {
        return err;
    }

    esp_flash_t* const chip = esp_flash_default_chip;

    // Три разных размера, и путать их нельзя:
    //   chip->size - сколько разрешено использовать (из заголовка образа);
    //   физический - сколько есть на самом деле;
    //   ID       - производитель и объём по JEDEC, для отчёта.
    uint32_t size = chip->size;
    uint32_t physical = 0;
    uint32_t id = 0;

    if (esp_flash_get_size(chip, &size) != ESP_OK) {
        size = chip->size;
    }
    // Обе функции необязательные для работы драйвера: если чип их не
    // поддержал, остаются нули, и это видно в отчёте щупа - лучше, чем
    // отказ инициализации из-за отчёта.
    (void)esp_flash_get_physical_size(chip, &physical);
    (void)esp_flash_read_id(chip, &id);

    s_size = size;
    s_physical_size = physical;
    s_id = id;
    s_ready = true;
    return 0;
}

bool basalt_flash_ready(void)
{
    return s_ready;
}

uint32_t basalt_flash_size(void)
{
    return s_size;
}

uint32_t basalt_flash_physical_size(void)
{
    return s_physical_size;
}

uint32_t basalt_flash_id(void)
{
    return s_id;
}

int basalt_flash_read(uint32_t address, void* dst, size_t length)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dst == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_flash_read(esp_flash_default_chip, dst, address, (uint32_t)length);
}

int basalt_flash_erase_sector(uint32_t address)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    // Проверка здесь, а не только в драйвере: драйвер вернёт INVALID_ARG и
    // на невыровненную длину, а нам важно, чтобы вызывающий не считал, что
    // стёрт ровно сектор.
    if ((address % BASALT_FLASH_SECTOR_SIZE) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_flash_erase_region(esp_flash_default_chip, address, BASALT_FLASH_SECTOR_SIZE);
}

int basalt_flash_write(uint32_t address, const void* src, size_t length)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (src == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // Стирание не делаем намеренно: «записать» и «стереть и записать» - разные
    // операции, и вторая не должна случаться от опечатки в адресе. Кто хочет
    // заменить содержимое, тот и стирает.
    return esp_flash_write(esp_flash_default_chip, src, address, (uint32_t)length);
}
