// env:lcd - первая прошивка, которая говорит с экраном.
//
// Задача вечера не «текстовый режим», а ответ на три вопроса к железу:
// панель вообще жива, шина собрана верно и цвета те, что задуманы. Поэтому
// здесь нет ни шрифта, ни курсора: если полосы вышли не того цвета или
// зеркально, искать причину в шрифте - худший способ отладки из возможных.
//
// Отдельный env, а не флаг в env:tdisplay, по той же причине, что у probe:
// env:tdisplay - зелёная сборка A0, и она должна оставаться зелёной, пока
// драйвер не доказан на плате. Замеры и решение идут в docs/board.md, §10.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"

#include "console_usb_jtag.h"
#include "format.h"
#include "lcd.h"
#include "start.h"
#include "tick.h"
#include "vectors.h"

// Кадровый буфер выровнен на строку кэша: когда заливка переедет на DMA,
// требование станет обязательным, а сейчас оно просто ничего не стоит.
#define FB_ALIGN 64u

// RGB565: пять бит красного, шесть зелёного, пять синего. Функция, а не
// макрос с магическими числами: по ней видно, откуда берутся сдвиги.
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8u) << 8) | ((uint16_t)(g & 0xFCu) << 3) | (uint16_t)(b >> 3));
}

// Восемь вертикальных полос по 40 пикселей. Набор подобран так, чтобы одна
// картинка отвечала сразу на все вопросы: красный и зелёный раздельно ловят
// перепутанные байты, белый и чёрный - инверсию, ровные границы полос -
// «мозаику» от слишком быстрого такта, а порядок полос - зеркало по оси X.
// Высота панели 170, поэтому полосы доходят до низа: по нижней кромке видно,
// не сдвинута ли развёртка по Y.
static void draw_bars(uint16_t* fb)
{
    const uint16_t bars[8] = {
        rgb565(255, 0, 0),     // красный
        rgb565(0, 255, 0),     // зелёный
        rgb565(0, 0, 255),     // синий
        rgb565(255, 255, 255), // белый
        rgb565(0, 0, 0),       // чёрный
        rgb565(255, 255, 0),   // жёлтый
        rgb565(0, 255, 255),   // голубой
        rgb565(255, 0, 255),   // пурпурный
    };

    const int width = lcd_width();
    const int height = lcd_height();
    const int bar_width = width / 8;

    for (int y = 0; y < height; ++y) {
        uint16_t* row = fb + (size_t)y * (size_t)width;
        for (int x = 0; x < width; ++x) {
            // Последний кусок достаётся последней полосе: width не обязана
            // делиться на восемь ровно.
            int index = x / bar_width;
            if (index > 7) {
                index = 7;
            }
            row[x] = bars[index];
        }
    }
}

// Рамка по краю экрана. Проверяет калибровку окна: если смещение или размер
// панели заданы неверно, рамка окажется толще с одной стороны или срежется
// краем - это видно сразу, в отличие от полос, у которых край просто «ещё одна
// полоса».
static void draw_border(uint16_t* fb, uint16_t color, int thickness)
{
    const int width = lcd_width();
    const int height = lcd_height();

    for (int y = 0; y < height; ++y) {
        uint16_t* row = fb + (size_t)y * (size_t)width;
        for (int x = 0; x < width; ++x) {
            if (x < thickness || y < thickness || x >= width - thickness || y >= height - thickness) {
                row[x] = color;
            }
        }
    }
}

// Квадрат в левом верхнем углу. Он отвечает на два вопроса сразу: где у
// панели «начало координат» (если квадрат оказался в другом углу - картинка
// зеркальна) и обновляется ли кадр повторно, а не остался от прошлой заливки.
static void draw_marker(uint16_t* fb, uint16_t color)
{
    const int width = lcd_width();
    for (int y = 8; y < 48; ++y) {
        for (int x = 8; x < 48; ++x) {
            fb[(size_t)y * (size_t)width + (size_t)x] = color;
        }
    }
}

// Заливка полного кадра с замером. Возвращает время в микросекундах: это и
// есть главное число вехи A1 - по нему считается, сколько кадров в секунду
// выдержит система.
static uint64_t flush_full(const uint16_t* fb)
{
    const uint64_t start = basalt_uptime_us();
    lcd_flush(0, 0, lcd_width(), lcd_height(), fb);
    return basalt_uptime_us() - start;
}

void basalt_main(void)
{
    // Порядок тот же, что и в остальных env: консоль первой - если что-то
    // ниже зависнет, след останется в логе. Пауза до вывода нужна, чтобы хост
    // успел открыть порт.
    basalt_console_init();
    basalt_tick_init();
    basalt_delay_us(2000000);
    basalt_vectors_init();

    const uint64_t t_boot = basalt_uptime_us();

    basalt_put("\n\nbasalt-os lcd\n");

    lcd_init();
    lcd_backlight(true);

    const int width = lcd_width();
    const int height = lcd_height();
    const size_t fb_bytes = (size_t)width * (size_t)height * 2u;

    basalt_put("[lcd] панель ");
    basalt_put_u64((uint64_t)width);
    basalt_put("x");
    basalt_put_u64((uint64_t)height);
    basalt_put(", RGB565, подсветка включена\n");

    uint16_t* const fb = heap_caps_aligned_alloc(FB_ALIGN, fb_bytes, MALLOC_CAP_SPIRAM);
    if (fb == NULL) {
        // Единственный честный отказ: без кадра рисовать нечего, а делать вид,
        // что экран работает, хуже, чем сказать прямо.
        basalt_put("[lcd] кадровый буфер ");
        basalt_put_u64(fb_bytes);
        basalt_put(" Б в PSRAM не выделился - экран не трогаем\n");
        for (;;) {
            basalt_delay_us(1000000);
        }
    }

    basalt_put("[lcd] кадровый буфер ");
    basalt_put_u64(fb_bytes);
    basalt_put(" Б, адрес 0x");
    basalt_put_hex((uint32_t)(uintptr_t)fb, 8);
    basalt_put("\n");

    draw_bars(fb);
    draw_border(fb, rgb565(0, 0, 255), 2);
    draw_marker(fb, rgb565(255, 255, 255));

    const uint64_t frame_us = flush_full(fb);
    basalt_put("[lcd] полный кадр ");
    basalt_put_u64(frame_us);
    basalt_put(" мкс");
    if (frame_us > 0) {
        basalt_put(", до ");
        basalt_put_u64(1000000u / frame_us);
        basalt_put(" кадров/с");
    }
    basalt_put("\n[lcd] должно быть видно: синяя рамка по краю, 8 вертикальных полос, белый квадрат в левом верхнем углу\n");
    basalt_put("ready\n\n");

    // Дальше - только подтверждение, что система жива и кадр можно повторить
    // сколько угодно раз. Квадрат меняет цвет на каждом тике: если он
    // застыл, значит заливка прошла один раз и сломалась на второй.
    uint32_t tick = 0;
    uint64_t next = t_boot + 1000000u;
    bool marker_white = true;

    for (;;) {
        const uint64_t now = basalt_uptime_us();
        if (now >= next) {
            next += 1000000u;
            ++tick;

            marker_white = !marker_white;
            draw_marker(fb, marker_white ? rgb565(255, 255, 255) : rgb565(0, 0, 0));
            const uint64_t us = flush_full(fb);

            basalt_put("tick ");
            basalt_put_u64(tick);
            basalt_put("  uptime ");
            basalt_put_sec3(now);
            basalt_put(" s, кадр ");
            basalt_put_u64(us);
            basalt_put(" мкс\n");
        }

        basalt_delay_us(1000);
    }
}
