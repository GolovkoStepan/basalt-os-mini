cmake_minimum_required(VERSION 3.16.0)

# Файлы умолчаний sdkconfig: общий плюс файл платы.
#
# Зачем: PlatformIO передаёт IDF один sdkconfig.defaults на все env, а размер
# flash - свойство платы (16 МБ у T-Display-S3, 8 МБ у Cardputer). IDF умеет
# читать несколько файлов умолчаний подряд, и это единственное место, где
# выбор платы уже известен и ещё не поздно: BASALT_BOARD_TDISPLAY приходит из
# platformio.ini (board_build.cmake_extra_args), как и в src/CMakeLists.txt.
#
# Почему не -DSDKCONFIG_DEFAULTS в platformio.ini: там «;» - начало
# комментария, и список из двух файлов не записать.
#
# Цена: PlatformIO следит за временем правки только sdkconfig.defaults, поэтому
# после правки файла платы сгенерированный sdkconfig.<env> надо удалить руками
# (он в .gitignore) - иначе значение останется старым.
if(BASALT_BOARD_TDISPLAY)
    set(BASALT_SDKCONFIG_DEFAULTS_BOARD "sdkconfig.defaults.tdisplay")
else()
    set(BASALT_SDKCONFIG_DEFAULTS_BOARD "sdkconfig.defaults.cardputer")
endif()
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/${BASALT_SDKCONFIG_DEFAULTS_BOARD}"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(basalt-os-mini)
