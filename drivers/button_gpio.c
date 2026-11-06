// Кнопки T-Display-S3: пины и полярность. Зачем отдельный файл - см. button.h.

#include "button.h"

#include "board_pins.h"
#include "pin_gpio.h"

static unsigned pin_of(button_id id)
{
    switch (id) {
    case BUTTON_SECONDARY:
        return PIN_BUTTON_2;
    case BUTTON_PRIMARY:
    default:
        // Неизвестный id - это ошибка вызывающего, но падать из-за кнопки
        // нельзя: пусть читается основная, её хотя бы видно.
        return PIN_BUTTON_G0;
    }
}

void button_init(void)
{
    pin_gpio_input(PIN_BUTTON_G0, true);
    pin_gpio_input(PIN_BUTTON_2, true);
}

bool button_down(button_id id)
{
    // Кнопка замыкает пин на землю, поэтому нажатие - это ноль.
    return !pin_gpio_read(pin_of(id));
}
