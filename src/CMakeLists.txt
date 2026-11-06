# Список файлов A0 задан явно, а не glob'ом.
#
# Почему вручную: PlatformIO для framework=espidf генерирует этот файл сам с
# FILE(GLOB_RECURSE) по всему src/, и build_src_filter при этом игнорируется -
# в сборку попадают все каталоги разом. Когда в src/ лежали щупы на Arduino,
# сборка bare падала на #include <Arduino.h> из чужого файла.
#
# Явный список заодно фиксирует состав вехи: если файл не назван здесь, он не
# попадёт в прошивку. arch/ живёт вне src/ намеренно - это слой платформы,
# который потом заменяется целиком при форке (проект на рисковом RISC-V).

# Выбор платы. Значение приходит из platformio.ini через
# board_build.cmake_extra_args (переменная кэша CMake): у каждой сборки свой
# каталог .pio/build, поэтому кэш не пересекается.
#
# Почему так, а не #ifdef в коде: различие между платами здесь ровно одно -
# чем сигналить «система жива». Всё остальное одинаково, потому что уровень
# arch/ одинаков. Держать оба варианта в одном файле значило бы заставить
# код платформы знать про обе платы сразу - а он не должен.
if(BASALT_BOARD_TDISPLAY)
    set(BASALT_INDICATOR_SRC "../arch/xtensa/indicator_backlight.c")
    set(BASALT_BOARD_DIR     "../boards/tdisplay_s3")
    set(BASALT_LCD_SRC       "../drivers/lcd_i80.c")
else()
    set(BASALT_INDICATOR_SRC "../arch/xtensa/indicator_ws2812.c")
    set(BASALT_BOARD_DIR     "../boards/cardputer")
    set(BASALT_LCD_SRC       "")
endif()

# Приложение тоже выбирается сборкой: env:probe, env:lcd и env:text - тот же
# старт и та же консоль, но другой main.c. Второй копии arch/ при этом не
# появляется, и ни один общий файл не обрастает ветвлениями «а в щупе иначе»:
# различие ровно в одном файле.
#
# Текстовая матрица подключается только текстовому env. В env:bare её файлы не
# нужны, а проверенную сборку A0 незачем увеличивать мёртвым кодом: пересборка
# с другим env ничего не должна менять в уже работающей прошивке.
set(BASALT_TEXT_SRC "")
set(BASALT_TEXT_INC "")

if(BASALT_APP_PROBE)
    set(BASALT_APP_SRC "probe/main.c")
elseif(BASALT_APP_LCD)
    set(BASALT_APP_SRC "lcd/main.c")
elseif(BASALT_APP_TEXT)
    set(BASALT_APP_SRC "text/main.c")
    set(BASALT_TEXT_SRC
        "../text/matrix.c"
        "../text/utf8.c"
        "../text/font_5x7.c"
    )
    set(BASALT_TEXT_INC "../text")
else()
    set(BASALT_APP_SRC "bare/main.c")
endif()

idf_component_register(
    SRCS
        ${BASALT_APP_SRC}
        "../arch/xtensa/console_usb_jtag.c"
        "../arch/xtensa/tick.c"
        ${BASALT_INDICATOR_SRC}
        "../arch/xtensa/format.c"
        "../arch/xtensa/panic.c"
        "../arch/xtensa/vectors.c"
        "../arch/xtensa/vectors.S"
        "../arch/xtensa/start.c"
        "../arch/xtensa/start.S"
        "../arch/xtensa/heap.c"
        ${BASALT_LCD_SRC}
        ${BASALT_TEXT_SRC}
    INCLUDE_DIRS
        "../arch/xtensa/include"
        "../drivers"
        ${BASALT_BOARD_DIR}
        ${BASALT_TEXT_INC}
)
