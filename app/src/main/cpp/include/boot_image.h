#ifndef OPENXISO_BOOT_IMAGE_H
#define OPENXISO_BOOT_IMAGE_H

/*
 * boot_image.h — разбор загрузочного образа: El Torito (ISO 9660) и MBR (.img).
 * Чтение идёт через файловый дескриптор, который открывает Java через SAF.
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

typedef struct OpenXisoBootPlan {
    OpenXisoImageKind kind;
    uint32_t boot_lba;       /* ISO: номер 2048-байтного сектора загрузчика */
    uint16_t boot_sectors;   /* сколько 512-байтных секторов грузить в 7C00 */
    uint8_t drive;           /* DL, который увидит загрузчик */
    int el_torito;           /* 1 = найден валидный каталог El Torito */
} OpenXisoBootPlan;

/** Определяет тип образа и план загрузки. Возвращает 0 при успехе. */
int openxiso_boot_plan(int fd, long long size, OpenXisoBootPlan* plan,
                       char* error, size_t error_capacity);

/** Копирует загрузочный сектор в dest (обычно 0000:7C00). Возвращает число байт. */
int openxiso_boot_load(int fd, const OpenXisoBootPlan* plan,
                       uint8_t* dest, size_t dest_capacity);

/** Размер сектора, который эмулирует INT 13h для этого типа образа. */
uint32_t openxiso_disk_sector_size(OpenXisoImageKind kind);

/** Чтение count секторов с LBA. Возвращает число прочитанных байт или -1. */
int openxiso_disk_read(int fd, OpenXisoImageKind kind, uint64_t lba,
                       uint32_t count, uint8_t* buffer, size_t buffer_capacity);

/** Текст последней ошибки на русском языке. */
const char* openxiso_boot_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENXISO_BOOT_IMAGE_H */
