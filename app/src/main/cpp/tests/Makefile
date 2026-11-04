# Хостовые тесты C-ядра и эмулятора: обычный gcc, без Android SDK/NDK.
# Запуск из корня репозитория: make -C app/src/main/cpp/tests run
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
INCLUDES := -I../include
CORE_SOURCES := ../openxiso_core.c test_core.c
EMU_SOURCES := ../openxiso_core.c ../x86_cpu.c ../boot_image.c \
               ../legacy_bios.c ../pc_machine.c test_emulator.c
HEADERS := $(wildcard ../include/*.h)

.PHONY: all run clean

all: test_core test_emulator

test_core: $(CORE_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) $(CORE_SOURCES) -o test_core

test_emulator: $(EMU_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) $(EMU_SOURCES) -o test_emulator

run: all
	./test_core
	./test_emulator

clean:
	rm -f test_core test_emulator
