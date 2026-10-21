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

idf_component_register(
    SRCS
        "bare/main.c"
        "../arch/xtensa/console_usb_jtag.c"
        "../arch/xtensa/tick.c"
        "../arch/xtensa/led_ws2812.c"
        "../arch/xtensa/format.c"
        "../arch/xtensa/panic.c"
        "../arch/xtensa/vectors.c"
        "../arch/xtensa/vectors.S"
        "../arch/xtensa/start.c"
        "../arch/xtensa/start.S"
    INCLUDE_DIRS
        "../arch/xtensa/include"
)
