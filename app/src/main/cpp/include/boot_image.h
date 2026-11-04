#ifndef OPENXISO_BOOT_IMAGE_H
#define OPENXISO_BOOT_IMAGE_H

/*
 * boot_image.h — разбор загрузочного образа: El Torito (ISO 9660) и MBR (.img).
 * Чтение идёт через файловый дескриптор, который открывает Java через SAF.
 *
 * Важно различать режимы El Torito:
 *   no emulation (0) — гость видит сам CD, LBA в секторах по 2048 байт;
 *   floppy/HDD (1..4) — BIOS эмулирует дискету или жёсткий диск, LBA в
 *   секторах по 512 байт и отсчитывается ОТ НАЧАЛА загрузочного образа,
 *   а не от начала ISO. Именно так собраны ISO DimOS, MikeOS и им подобные.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum OpenXisoImageKind {
    OPENXISO_IMAGE_UNKNOWN = 0,
    OPENXISO_IMAGE_ISO = 1,
    OPENXISO_IMAGE_RAW = 2
} OpenXisoImageKind;

typedef enum OpenXisoEmulation {
    OPENXISO_EMULATION_NONE = 0,           /* CD без эмуляции, 2048 байт */
    OPENXISO_EMULATION_FLOPPY_1200 = 1,    /* дискета 1,2 МБ */
    OPENXISO_EMULATION_FLOPPY_1440 = 2,    /* дискета 1,44 МБ */
    OPENXISO_EMULATION_FLOPPY_2880 = 3,    /* дискета 2,88 МБ */
    OPENXISO_EMULATION_HARD_DISK = 4       /* эмуляция жёсткого диска */
} OpenXisoEmulation;

typedef struct OpenXisoBootPlan {
    OpenXisoImageKind kind;
    uint32_t boot_lba;        /* ISO: номер 2048-байтного сектора образа */
    uint16_t boot_sectors;    /* сколько 512-байтных секторов грузить в 7C00 */
    uint16_t load_segment;    /* сегмент загрузки из initial entry */
    uint8_t drive;            /* DL, который увидит загрузчик */
    uint8_t emulation;        /* OpenXisoEmulation */
    int el_torito;            /* 1 = найден валидный каталог El Torito */

    uint64_t boot_offset;     /* откуда читать загрузочный сектор, байты */
    uint64_t image_offset;    /* база LBA для INT 13h, байты */
    uint64_t image_bytes;     /* размер эмулируемого диска, байты */
    uint32_t sector_size;     /* 512 или 2048 */
    uint16_t sectors_per_track;
    uint16_t heads;
} OpenXisoBootPlan;

/** Определяет тип образа и план загрузки. Возвращает 0 при успехе. */
int openxiso_boot_plan(int fd, long long size, OpenXisoBootPlan* plan,
                       char* error, size_t error_capacity);

/** Копирует загрузочный сектор в dest (обычно 0000:7C00). Возвращает число байт. */
int openxiso_boot_load(int fd, const OpenXisoBootPlan* plan,
                       uint8_t* dest, size_t dest_capacity);

/** Размер сектора, который эмулирует INT 13h для этого плана. */
uint32_t openxiso_disk_sector_size(const OpenXisoBootPlan* plan);

/** Полное число секторов эмулируемого диска (для INT 13h AH=48h). */
uint64_t openxiso_disk_total_sectors(const OpenXisoBootPlan* plan);

/** Чтение count секторов с LBA. Возвращает число прочитанных байт или -1. */
int openxiso_disk_read(int fd, const OpenXisoBootPlan* plan, uint64_t lba,
                       uint32_t count, uint8_t* buffer, size_t buffer_capacity);

/** Русскоязычное название режима эмуляции для журнала POST. */
const char* openxiso_emulation_name(uint8_t emulation);

/** Текст последней ошибки на русском языке. */
const char* openxiso_boot_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENXISO_BOOT_IMAGE_H */
