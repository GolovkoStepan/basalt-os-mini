// Заведение кучи при своём старте. Зачем это здесь - см. heap.h.

#include <stdbool.h>

#include "heap.h"

#include "esp_heap_caps_init.h"
#include "esp_private/esp_psram_extram.h"
#include "esp_psram.h"

// Два вызова, а не один: в IDF это две разные ESP_SYSTEM_INIT_FN с
// приоритетами 100 и 103. Первая регистрирует в куче области SRAM, которые
// ядро оставило свободными, вторая добавляет к ним PSRAM. Порядок важен -
// добавлять PSRAM в несуществующую кучу некуда.
//
// Публичной функции «добавь PSRAM в кучу» в IDF нет: esp_psram_init() только
// размечает и отображает память, а в аллокатор её кладёт приватная
// esp_psram_extram_add_to_heap_allocator(). Приватный заголовок здесь -
// осознанная цена: поведение ровно то же, что у IDF, и цифры в памяти
// сравнимы с теми, что печатает штатная прошивка.
void basalt_heap_init(void)
{
    heap_caps_init();

    // Проверка вместо CONFIG_SPIRAM_IGNORE_NOTFOUND: файл sdkconfig один на
    // обе платы, и на Cardputer PSRAM физически нет. Там эта ветка просто не
    // выполнится, и плата поднимется как раньше - без всякой условной
    // компиляции в общем коде.
    if (esp_psram_is_initialized()) {
        esp_psram_extram_add_to_heap_allocator();
    }
}
