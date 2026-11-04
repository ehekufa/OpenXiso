# Хостовые тесты C-ядра: обычный gcc, без Android SDK/NDK.
# Запуск из корня репозитория: make -C app/src/main/cpp/tests
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
INCLUDES := -I../include
SOURCES := ../openxiso_core.c test_core.c
TARGET := test_core

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCES) ../include/openxiso_core.h
	$(CC) $(CFLAGS) $(INCLUDES) $(SOURCES) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
