#define _POSIX_C_SOURCE 200809L

/*
 * boot_image.c — собственный разбор El Torito и загрузка MBR.
 *
 * ISO 9660: первичный дескриптор тома в секторе 16, дескриптор загрузочной
 * записи в секторе 17 (указатель на каталог El Torito по смещению 0x47),
 * затем проверочная запись каталога (0x55AA) и начальная запись с LBA образа.
 * Для .img грузится обычный MBR из нулевого сектора.
 */

#include "include/boot_image.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char last_error[192] = "";

static void set_error(const char* message) {
    snprintf(last_error, sizeof(last_error), "%s", message);
}

const char* openxiso_boot_last_error(void) {
    return last_error[0] == '\0' ? "Причина не установлена" : last_error;
}

static uint32_t read_le32(const uint8_t* bytes) {
    return (uint32_t)bytes[0]
           | ((uint32_t)bytes[1] << 8)
           | ((uint32_t)bytes[2] << 16)
           | ((uint32_t)bytes[3] << 24);
}

static uint16_t read_le16(const uint8_t* bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int read_at(int fd, void* buffer, size_t length, long long offset) {
    if (fd < 0 || length == 0) {
        return -1;
    }
    const ssize_t result = pread(fd, buffer, length, (off_t)offset);
    return result == (ssize_t)length ? 0 : -1;
}

int openxiso_boot_plan(int fd, long long size, OpenXisoBootPlan* plan,
                       char* error, size_t error_capacity) {
    last_error[0] = '\0';
    if (plan == NULL || fd < 0 || size <= 0) {
        set_error("Не удалось открыть файл образа");
        if (error && error_capacity) {
            snprintf(error, error_capacity, "%s", last_error);
        }
        return -1;
    }

    memset(plan, 0, sizeof(*plan));
    plan->boot_sectors = 1;
    plan->drive = 0x80;

    uint8_t sector[2048];
    if (read_at(fd, sector, sizeof(sector), 16LL * 2048LL) == 0 &&
        sector[0] == 1 && memcmp(sector + 1, "CD001", 5) == 0) {
        plan->kind = OPENXISO_IMAGE_ISO;

        uint8_t boot_record[2048];
        if (read_at(fd, boot_record, sizeof(boot_record), 17LL * 2048LL) != 0 ||
            boot_record[0] != 0 || memcmp(boot_record + 1, "CD001", 5) != 0) {
            set_error("Дескриптор загрузочной записи ISO 9660 не найден");
            if (error && error_capacity) {
                snprintf(error, error_capacity, "%s", last_error);
            }
            return -1;
        }

        const uint32_t catalog_lba = read_le32(boot_record + 0x47);
        if (catalog_lba == 0) {
            set_error("Каталог El Torito не объявлен в образе");
            if (error && error_capacity) {
                snprintf(error, error_capacity, "%s", last_error);
            }
            return -1;
        }

        uint8_t catalog[2048];
        if (read_at(fd, catalog, sizeof(catalog), (long long)catalog_lba * 2048LL) != 0) {
            set_error("Не удалось прочитать каталог El Torito");
            if (error && error_capacity) {
                snprintf(error, error_capacity, "%s", last_error);
            }
            return -1;
        }
        if (catalog[0] != 1 || catalog[0x1E] != 0x55 || catalog[0x1F] != 0xAA) {
            set_error("Проверочная запись каталога El Torito повреждена");
            if (error && error_capacity) {
                snprintf(error, error_capacity, "%s", last_error);
            }
            return -1;
        }

        const uint8_t* entry = catalog + 32;
        if (entry[0] != 0x88 && entry[0] != 0x00) {
            set_error("В каталоге El Torito нет загрузочной записи");
            if (error && error_capacity) {
                snprintf(error, error_capacity, "%s", last_error);
            }
            return -1;
        }
        plan->boot_lba = read_le32(entry + 8);
        plan->boot_sectors = read_le16(entry + 12);
        if (plan->boot_sectors == 0 || plan->boot_sectors > 8) {
            plan->boot_sectors = 4; /* стандарт El Torito: 2048 байт */
        }
        plan->drive = 0xE0;
        plan->el_torito = 1;
        return 0;
    }

    /* Сырой образ .img: проверяем сигнатуру MBR. */
    if (read_at(fd, sector, 512, 0) != 0 || sector[510] != 0x55 || sector[511] != 0xAA) {
        set_error("Не найдена загрузочная запись MBR (0x55AA)");
        if (error && error_capacity) {
            snprintf(error, error_capacity, "%s", last_error);
        }
        return -1;
    }
    plan->kind = OPENXISO_IMAGE_RAW;
    plan->boot_lba = 0;
    plan->boot_sectors = 1;
    plan->drive = 0x80;
    return 0;
}

int openxiso_boot_load(int fd, const OpenXisoBootPlan* plan,
                       uint8_t* dest, size_t dest_capacity) {
    if (plan == NULL || dest == NULL || fd < 0) {
        return -1;
    }
    const size_t length = (size_t)plan->boot_sectors * 512u;
    if (length > dest_capacity) {
        set_error("Загрузочный сектор не помещается в область 0000:7C00");
        return -1;
    }
    const long long offset = plan->kind == OPENXISO_IMAGE_ISO
                             ? (long long)plan->boot_lba * 2048LL
                             : 0LL;
    if (read_at(fd, dest, length, offset) != 0) {
        set_error("Не удалось прочитать загрузочный сектор образа");
        return -1;
    }
    return (int)length;
}

uint32_t openxiso_disk_sector_size(OpenXisoImageKind kind) {
    return kind == OPENXISO_IMAGE_ISO ? 2048u : 512u;
}

int openxiso_disk_read(int fd, OpenXisoImageKind kind, uint64_t lba,
                       uint32_t count, uint8_t* buffer, size_t buffer_capacity) {
    if (fd < 0 || buffer == NULL || count == 0) {
        return -1;
    }
    const uint32_t sector_size = openxiso_disk_sector_size(kind);
    const size_t length = (size_t)count * sector_size;
    if (length > buffer_capacity) {
        set_error("Запрос INT 13h превышает размер буфера");
        return -1;
    }
    const long long offset = (long long)(lba * sector_size);
    const ssize_t result = pread(fd, buffer, length, (off_t)offset);
    if (result < 0) {
        set_error("Ошибка чтения образа через файловый дескриптор");
        return -1;
    }
    if ((size_t)result < length) {
        memset(buffer + result, 0, length - (size_t)result);
    }
    return (int)length;
}
