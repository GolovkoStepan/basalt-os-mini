// Заглушка консоли: то, чем тест подменяет USB Serial/JTAG.
#pragma once

// Очистить накопленный вывод.
void console_stub_reset(void);

// Что «напечаталось» с момента сброса.
const char* console_stub_text(void);
