# Короткие имена для команд, которые иначе приходится помнить.
#
# Зачем файл: команд немного, но у половины есть env, а половина живёт в
# tools/ своими скриптами. Помнить каждый вечер, какой из пяти env собирать
# перед проверкой образа и чем читать лог, - работа, которая ничего не даёт и
# может закончиться не тем env. Здесь только имена: вся логика остаётся в
# platformio.ini и tools/, чтобы не завести второй источник правды.
#
# ENV по умолчанию bare, а не console. bare - свой код, то есть проект.
# console - прошивка «проверить, что плата отвечает»; default_envs в
# platformio.ini остаётся console, потому что pio run без аргументов удобно
# тыкать в плату, когда она ведёт себя странно.
#
# Грабли: make flash ENV=hello заливает щуп, после которого плата перестаёт
# отвечать esptool до ручного G0 (docs/board.md, §9). Собирать его при этом
# можно и нужно - он входит в verify.
#
# pio остаётся доступен напрямую: make - сокращение, а не замена. Если сборка
# упирается в питон, на котором стоит PlatformIO, его можно подменить:
#   make build PIO=~/.asdf/installs/python/3.14.2/bin/pio

ENV ?= bare
SECONDS ?= 15
PIO ?= pio

# Список env для verify. Должен совпадать с .github/workflows/build.yml:
# сломанный env, который собирается только вручную, обнаруживают через неделю.
ENVS ?= console hello bringup bare bare-panic

# Порт нужен, только если в системе больше одной платы.
PORT_ARG = $(if $(PORT),--upload-port $(PORT),)

.PHONY: all help build flash log monitor test check verify smoke compiledb \
        reset erase clean id backup devices

all: build

help: ## этот список
	@awk 'BEGIN {FS = ":.*## "} /^[a-zA-Z_-]+:.*## / {printf "  make %-11s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

build: ## собрать ENV (по умолчанию bare)
	$(PIO) run -e $(ENV)

flash: ## собрать и залить; порт: make flash PORT=/dev/cu.usbmodem101
	$(PIO) run -e $(ENV) -t upload $(PORT_ARG)

log: ## читать лог платы SECONDS секунд (по умолчанию 15)
	./tools/serial-read.sh $(SECONDS)

monitor: ## интерактивный монитор; годится только в живом терминале
	$(PIO) device monitor -e $(ENV)

test: ## тесты на маке, плата не нужна
	$(PIO) test -e native

check: ## проверить образ bare по символам, плата не нужна
	./tools/check-image.sh

verify: ## то же, что CI: тесты, сборка всех env, проверка образа
	$(PIO) test -e native
	$(PIO) run $(addprefix -e ,$(ENVS))
	./tools/check-image.sh

smoke: ## залить ENV и сверить лог с «признаком успеха» (плата нужна)
	./tools/smoke.sh $(ENV)

compiledb: ## база для clangd под ENV; под другую env - отдельным вызовом
	$(PIO) run -e $(ENV) -t compiledb

reset: ## вывести плату из загрузчика сбросом через USB-JTAG
	./tools/reset.sh $(PORT)

erase: ## стереть flash целиком
	$(PIO) run -e $(ENV) -t erase

clean: ## удалить сборку ENV
	$(PIO) run -e $(ENV) -t clean

id: ## чип, размер flash, MAC
	./tools/flash-backup.sh flash-id

backup: ## сохранить оригинальную прошивку в backup/
	./tools/flash-backup.sh backup

devices: ## список последовательных портов
	$(PIO) device list
