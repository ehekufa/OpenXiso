#define _POSIX_C_SOURCE 200809L

/*
 * boot_image.c — собственный разбор El Torito и загрузка MBR.
 *
 * ISO 9660: первичный дескриптор тома в секторе 16, дескриптор загрузочной
 * записи в секторе 17 (указатель на каталог El Torito по смещению 0x47),
 * затем проверочная запись каталога (0x55AA) и начальная запись с LBA образа.
 * Для .img грузится обычный MBR из нулевого сектора.
 *
 * Байт 1 начальной записи каталога задаёт тип эмуляции. Для дискеты/жёсткого
 * диска BIOS обязан показывать гостю НЕ весь ISO, а только сам загрузочный
 * образ, с секторами по 512 байт. Геометрию берём из BPB этого образа, как
 * это делает настоящая прошивка при floppy emulation.
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

const char* openxiso_emulation_name(uint8_t emulation) {
    switch (emulation) {
        case OPENXISO_EMULATION_FLOPPY_1200: return "эмуляция дискеты 1,2 МБ";
        case OPENXISO_EMULATION_FLOPPY_1440: return "эмуляция дискеты 1,44 МБ";
        case OPENXISO_EMULATION_FLOPPY_2880: return "эмуляция дискеты 2,88 МБ";
        case OPENXISO_EMULATION_HARD_DISK: return "эмуляция жёсткого диска";
        default: return "CD без эмуляции";
    }
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

static uint64_t minimum_u64(uint64_t left, uint64_t right) {
    return left < right ? left : right;
}

/** Читает ровно length байт; короткое чтение дополняется нулями. */
static int read_at(int fd, void* buffer, size_t length, long long offset) {
    if (fd < 0 || length == 0 || offset < 0) {
        return -1;
    }
    uint8_t* destination = (uint8_t*)buffer;
    size_t done = 0;
    while (done < length) {
        const ssize_t result = pread(fd, destination + done, length - done,
                                     (off_t)offset + (off_t)done);
        if (result < 0) {
            return -1;
        }
        if (result == 0) {
            break;
        }
        done += (size_t)result;
    }
    if (done < length) {
        memset(destination + done, 0, length - done);
    }
    return 0;
}

static void apply_bpb(int fd, uint64_t boot_offset, OpenXisoBootPlan* plan) {
    uint8_t sector[512];
    if (read_at(fd, sector, sizeof(sector), (long long)boot_offset) != 0) {
        return;
    }
    /* BPB допустим только при объявленном размере сектора 512 байт. */
    if (read_le16(sector + 0x0Bu) != 512u) {
        return;
    }
    const uint16_t sectors_per_track = read_le16(sector + 0x18u);
    const uint16_t heads = read_le16(sector + 0x1Au);
    if (sectors_per_track >= 1u && sectors_per_track <= 63u) {
        plan->sectors_per_track = sectors_per_track;
    }
    if (heads >= 1u && heads <= 255u) {
        plan->heads = heads;
    }
    /* Сырой .img с медиа-байтом дискеты должен получить DL=00h. */
    if (plan->kind == OPENXISO_IMAGE_RAW) {
        const uint8_t media = sector[0x15u];
        if (media >= 0xF0u) {
            plan->drive = 0x00u;
        }
    }
}

static int fail_plan(const char* message, char* error, size_t error_capacity) {
    set_error(message);
    if (error != NULL && error_capacity != 0) {
        snprintf(error, error_capacity, "%s", last_error);
    }
    return -1;
}

int openxiso_boot_plan(int fd, long long size, OpenXisoBootPlan* plan,
                       char* error, size_t error_capacity) {
    last_error[0] = '\0';
    if (plan == NULL || fd < 0 || size <= 0) {
        return fail_plan("Не удалось открыть файл образа", error, error_capacity);
    }

    memset(plan, 0, sizeof(*plan));
    plan->boot_sectors = 1;
    plan->drive = 0x80;
    plan->sector_size = 512;
    plan->sectors_per_track = 63;
    plan->heads = 16;
    plan->image_bytes = (uint64_t)size;

    uint8_t sector[2048];
    if (read_at(fd, sector, sizeof(sector), 16LL * 2048LL) == 0 &&
        sector[0] == 1 && memcmp(sector + 1, "CD001", 5) == 0) {
        plan->kind = OPENXISO_IMAGE_ISO;

        uint8_t boot_record[2048];
        if (read_at(fd, boot_record, sizeof(boot_record), 17LL * 2048LL) != 0 ||
            boot_record[0] != 0 || memcmp(boot_record + 1, "CD001", 5) != 0) {
            return fail_plan("Дескриптор загрузочной записи ISO 9660 не найден",
                             error, error_capacity);
        }

        const uint32_t catalog_lba = read_le32(boot_record + 0x47);
        if (catalog_lba == 0) {
            return fail_plan("Каталог El Torito не объявлен в образе",
                             error, error_capacity);
        }

        uint8_t catalog[2048];
        if (read_at(fd, catalog, sizeof(catalog),
                    (long long)catalog_lba * 2048LL) != 0) {
            return fail_plan("Не удалось прочитать каталог El Torito",
                             error, error_capacity);
        }
        if (catalog[0] != 1 || catalog[0x1E] != 0x55 || catalog[0x1F] != 0xAA) {
            return fail_plan("Проверочная запись каталога El Torito повреждена",
                             error, error_capacity);
        }

        const uint8_t* entry = catalog + 32;
        if (entry[0] != 0x88 && entry[0] != 0x00) {
            return fail_plan("В каталоге El Torito нет загрузочной записи",
                             error, error_capacity);
        }
        plan->boot_lba = read_le32(entry + 8);
        plan->boot_sectors = read_le16(entry + 12);
        if (plan->boot_sectors == 0 || plan->boot_sectors > 8) {
            plan->boot_sectors = 4; /* стандарт El Torito: 2048 байт */
        }
        plan->load_segment = read_le16(entry + 2);
        plan->el_torito = 1;
        plan->emulation = entry[1] > OPENXISO_EMULATION_HARD_DISK
                          ? (uint8_t)OPENXISO_EMULATION_NONE : entry[1];
        plan->boot_offset = (uint64_t)plan->boot_lba * 2048u;
        if (plan->boot_offset >= (uint64_t)size) {
            return fail_plan("Загрузочный образ лежит за пределами ISO",
                             error, error_capacity);
        }

        const uint64_t available = (uint64_t)size - plan->boot_offset;
        switch (plan->emulation) {
            case OPENXISO_EMULATION_FLOPPY_1200:
                plan->image_bytes = minimum_u64(available, 1200u * 1024u);
                plan->drive = 0x00u;
                plan->sectors_per_track = 15u;
                plan->heads = 2u;
                break;
            case OPENXISO_EMULATION_FLOPPY_1440:
                plan->image_bytes = minimum_u64(available, 1440u * 1024u);
                plan->drive = 0x00u;
                plan->sectors_per_track = 18u;
                plan->heads = 2u;
                break;
            case OPENXISO_EMULATION_FLOPPY_2880:
                plan->image_bytes = minimum_u64(available, 2880u * 1024u);
                plan->drive = 0x00u;
                plan->sectors_per_track = 36u;
                plan->heads = 2u;
                break;
            case OPENXISO_EMULATION_HARD_DISK:
                plan->image_bytes = available;
                plan->drive = 0x80u;
                break;
            default:
                /* CD без эмуляции: гость читает весь ISO секторами по 2048. */
                plan->emulation = OPENXISO_EMULATION_NONE;
                plan->image_offset = 0u;
                plan->image_bytes = (uint64_t)size;
                plan->sector_size = 2048u;
                plan->drive = 0xE0u;
                break;
        }
        if (plan->emulation != OPENXISO_EMULATION_NONE) {
            plan->image_offset = plan->boot_offset;
            plan->sector_size = 512u;
            apply_bpb(fd, plan->boot_offset, plan);
        }
        return 0;
    }

    /* Сырой образ .img: проверяем сигнатуру MBR / загрузочного сектора. */
    if (read_at(fd, sector, 512, 0) != 0 || sector[510] != 0x55 || sector[511] != 0xAA) {
        return fail_plan("Не найдена загрузочная запись MBR (0x55AA)",
                         error, error_capacity);
    }
    plan->kind = OPENXISO_IMAGE_RAW;
    plan->boot_lba = 0;
    plan->boot_sectors = 1;
    plan->drive = 0x80u;
    plan->boot_offset = 0u;
    plan->image_offset = 0u;
    plan->image_bytes = (uint64_t)size;
    plan->sector_size = 512u;
    apply_bpb(fd, 0u, plan);
    return 0;
}

int openxiso_boot_load(int fd, const OpenXisoBootPlan* plan,
                       uint8_t* dest, size_t dest_capacity) {
    if (plan == NULL || dest == NULL || fd < 0) {
        return -1;
    }
    /*
     * При эмуляции BIOS грузит ровно первый 512-байтный сектор образа:
     * остальной диск гость читает сам через INT 13h.
     */
    const size_t length = (plan->el_torito &&
                           plan->emulation == OPENXISO_EMULATION_NONE)
                          ? (size_t)plan->boot_sectors * 512u
                          : 512u;
    if (length > dest_capacity) {
        set_error("Загрузочный сектор не помещается в область 0000:7C00");
        return -1;
    }
    if (read_at(fd, dest, length, (long long)plan->boot_offset) != 0) {
        set_error("Не удалось прочитать загрузочный сектор образа");
        return -1;
    }
    return (int)length;
}

uint32_t openxiso_disk_sector_size(const OpenXisoBootPlan* plan) {
    return plan == NULL || plan->sector_size == 0 ? 512u : plan->sector_size;
}

uint64_t openxiso_disk_total_sectors(const OpenXisoBootPlan* plan) {
    if (plan == NULL || plan->image_bytes == 0) {
        return 0;
    }
    return plan->image_bytes / openxiso_disk_sector_size(plan);
}

int openxiso_disk_read(int fd, const OpenXisoBootPlan* plan, uint64_t lba,
                       uint32_t count, uint8_t* buffer, size_t buffer_capacity) {
    if (fd < 0 || plan == NULL || buffer == NULL || count == 0) {
        return -1;
    }
    const uint32_t sector_size = openxiso_disk_sector_size(plan);
    const size_t length = (size_t)count * sector_size;
    if (length > buffer_capacity) {
        set_error("Запрос INT 13h превышает размер буфера");
        return -1;
    }
    const uint64_t relative = lba * sector_size;
    if (plan->image_bytes != 0 &&
        (relative > plan->image_bytes || length > plan->image_bytes - relative)) {
        set_error("Чтение INT 13h за пределами эмулируемого диска");
        return -1;
    }
    const long long offset = (long long)(plan->image_offset + relative);
    if (offset < 0 || read_at(fd, buffer, length, offset) != 0) {
        set_error("Ошибка чтения образа через файловый дескриптор");
        return -1;
    }
    return (int)length;
}
