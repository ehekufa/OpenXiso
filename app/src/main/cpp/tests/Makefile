# Хостовые тесты C-ядра и эмулятора: обычный gcc, без Android SDK/NDK.
# Запуск из корня репозитория: make -C app/src/main/cpp/tests run
.DEFAULT_GOAL := all
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
INCLUDES := -I../include
CORE_SOURCES := ../openxiso_core.c test_core.c
EMU_SOURCES := ../openxiso_core.c ../x86_cpu.c ../x86_wide.c ../boot_image.c \
               ../legacy_bios.c ../pc_machine.c ../devices.c test_emulator.c
WIDE_SOURCES := ../openxiso_core.c ../x86_cpu.c ../x86_wide.c ../boot_image.c \
                ../legacy_bios.c ../pc_machine.c ../devices.c test_wide.c
HEADERS := $(wildcard ../include/*.h)

.PHONY: all run clean

linux_probe: ../openxiso_core.c ../x86_cpu.c ../x86_wide.c ../boot_image.c ../legacy_bios.c ../pc_machine.c ../devices.c linux_probe.c $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) ../openxiso_core.c ../x86_cpu.c ../x86_wide.c ../boot_image.c ../legacy_bios.c ../pc_machine.c ../devices.c linux_probe.c -o $@

all: test_core test_emulator test_wide test_devices

test_core: $(CORE_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) $(CORE_SOURCES) -o test_core

test_emulator: $(EMU_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) $(EMU_SOURCES) -o test_emulator

test_wide: $(WIDE_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) $(WIDE_SOURCES) -o test_wide

test_devices: ../openxiso_core.c ../x86_cpu.c ../x86_wide.c ../boot_image.c ../legacy_bios.c ../pc_machine.c ../devices.c test_devices.c $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) ../openxiso_core.c ../x86_cpu.c ../x86_wide.c ../boot_image.c ../legacy_bios.c ../pc_machine.c ../devices.c test_devices.c -o $@

run: all
	./test_core
	./test_emulator
	./test_wide
	./test_devices

clean:
	rm -f test_core test_emulator test_wide test_devices linux_probe
