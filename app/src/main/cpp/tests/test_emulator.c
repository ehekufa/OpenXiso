/*
 * test_emulator.c — проверка НАСТОЯЩЕЙ загрузки, а не только интерфейса.
 *
 * Собирает синтетический ISO 9660 с корректным каталогом El Torito и 16-битным
 * загрузочным сектором (машинные коды 8086), а также образ .img с MBR. Дальше
 * запускает виртуальный PC: POST собственного Legacy BIOS, INT 19h, исполнение
 * загрузчика, печать через INT 10h, чтение диска через INT 13h AH=42h и HLT.
 */

#include "legacy_bios.h"
#include "pc_machine.h"
#include "x86_cpu.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

#define CHECK(condition, message)                                            \
    do {                                                                     \
        ++checks;                                                            \
        if (!(condition)) {                                                   \
            ++failures;                                                       \
            printf("ОШИБКА: %s (строка %d)\n", (message), __LINE__);          \
        }                                                                     \
    } while (0)

/* ---------------- Сборка 16-битного загрузчика ---------------- */

static uint8_t boot_code[512];

static void build_boot_program(const char* message, int with_disk) {
    memset(boot_code, 0, sizeof(boot_code));
    int p = 0;
    int patch_msg1 = 0, patch_call1 = 0, patch_dap = 0, patch_jc = 0;
    int patch_msg2 = 0, patch_call2 = 0, patch_msg3 = 0, patch_call3 = 0;
    int patch_jz = 0, patch_jmp = 0;
    int msg1 = 0, msg2 = 0, msg3 = 0, dap = 0, print = 0, done = 0, error = 0;

#define EMIT(byte) (boot_code[p++] = (uint8_t)(byte))
#define WORD(value) do { EMIT((value) & 0xFF); EMIT(((value) >> 8) & 0xFF); } while (0)
#define WORD_AT(position, value) do {                                        \
        boot_code[(position)] = (uint8_t)((value) & 0xFF);                  \
        boot_code[(position) + 1] = (uint8_t)(((value) >> 8) & 0xFF);      \
    } while (0)
#define DWORD(value) do {                                                    \
        EMIT((value) & 0xFF); EMIT(((value) >> 8) & 0xFF);                  \
        EMIT(((value) >> 16) & 0xFF); EMIT(((value) >> 24) & 0xFF);         \
    } while (0)

    EMIT(0xBE); patch_msg1 = p; WORD(0);            /* mov si, msg1 */
    EMIT(0xE8); patch_call1 = p; WORD(0);           /* call print   */

    if (with_disk) {
        EMIT(0xB4); EMIT(0x42);                     /* mov ah, 42h  */
        EMIT(0xB2); EMIT(0xE0);                     /* mov dl, E0h  */
        EMIT(0xBE); patch_dap = p; WORD(0);         /* mov si, dap  */
        EMIT(0xCD); EMIT(0x13);                     /* int 13h      */
        EMIT(0x72); patch_jc = p; EMIT(0);          /* jc error     */
        EMIT(0xBE); patch_msg2 = p; WORD(0);        /* mov si, msg2 */
        EMIT(0xE8); patch_call2 = p; WORD(0);       /* call print   */
        EMIT(0xF4);                                 /* hlt          */
        error = p;
        EMIT(0xBE); patch_msg3 = p; WORD(0);        /* mov si, msg3 */
        EMIT(0xE8); patch_call3 = p; WORD(0);       /* call print   */
        EMIT(0xF4);                                 /* hlt          */
    } else {
        EMIT(0xF4);                                 /* hlt          */
        error = p;
    }

    print = p;
    EMIT(0xAC);                                     /* lodsb        */
    EMIT(0x84); EMIT(0xC0);                         /* test al, al  */
    EMIT(0x74); patch_jz = p; EMIT(0);              /* jz done      */
    EMIT(0xB4); EMIT(0x0E);                         /* mov ah, 0Eh  */
    EMIT(0xCD); EMIT(0x10);                         /* int 10h      */
    EMIT(0xEB); patch_jmp = p; EMIT(0);             /* jmp print    */
    done = p;
    EMIT(0xC3);                                     /* ret          */

    msg1 = p;
    for (const char* c = message; *c; ++c) {
        EMIT(*c);
    }
    EMIT(0);
    msg2 = p;
    for (const char* c = "El Torito read OK\r\n"; *c; ++c) {
        EMIT(*c);
    }
    EMIT(0);
    msg3 = p;
    for (const char* c = "Disk error\r\n"; *c; ++c) {
        EMIT(*c);
    }
    EMIT(0);

    while (p & 1) {
        EMIT(0);
    }
    dap = p;
    EMIT(0x10); EMIT(0x00);                         /* размер DAP   */
    WORD(1);                                        /* 1 сектор     */
    WORD(0x8000); WORD(0x0000);                     /* 0000:8000    */
    DWORD(19); DWORD(0);                            /* LBA = 19     */

    WORD_AT(patch_msg1, 0x7C00 + msg1);
    WORD_AT(patch_call1, print - (patch_call1 + 2));
    if (with_disk) {
        WORD_AT(patch_dap, 0x7C00 + dap);
        boot_code[patch_jc] = (uint8_t)(error - (patch_jc + 1));
        WORD_AT(patch_msg2, 0x7C00 + msg2);
        WORD_AT(patch_call2, print - (patch_call2 + 2));
        WORD_AT(patch_msg3, 0x7C00 + msg3);
        WORD_AT(patch_call3, print - (patch_call3 + 2));
    }
    boot_code[patch_jz] = (uint8_t)(done - (patch_jz + 1));
    boot_code[patch_jmp] = (uint8_t)(print - (patch_jmp + 1));

#undef EMIT
#undef WORD
#undef WORD_AT
#undef DWORD
}

/*
 * Второй загрузчик: mov ah,00h; int 16h; mov ah,0Eh; int 10h; hlt.
 * Проверяет, что BIOS честно переводит машину в ожидание клавиши и продолжает
 * исполнение ровно с инструкции INT после того, как Java положит символ в очередь.
 */
static void build_keyboard_program(void) {
    static const uint8_t code[] = {
        0xB4, 0x00,             /* mov ah, 00h */
        0xCD, 0x16,             /* int 16h     */
        0xB4, 0x0E,             /* mov ah, 0Eh */
        0xCD, 0x10,             /* int 10h     */
        0xF4                    /* hlt         */
    };
    memset(boot_code, 0, sizeof(boot_code));
    memcpy(boot_code, code, sizeof(code));
}

/* ---------------- Синтетические образы ---------------- */

static void put_le32(uint8_t* destination, uint32_t value) {
    destination[0] = (uint8_t)(value & 0xFFu);
    destination[1] = (uint8_t)((value >> 8) & 0xFFu);
    destination[2] = (uint8_t)((value >> 16) & 0xFFu);
    destination[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static int write_iso(const char* path) {
    const size_t size = 20u * 2048u;
    uint8_t* image = (uint8_t*)calloc(size, 1u);
    if (image == NULL) {
        return 0;
    }

    uint8_t* pvd = image + 16u * 2048u;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memcpy(pvd + 40, "OPENXISO LEGACY DISC", 20);
    put_le32(pvd + 80, 100u);

    uint8_t* boot_record = image + 17u * 2048u;
    boot_record[0] = 0;
    memcpy(boot_record + 1, "CD001", 5);
    boot_record[6] = 1;
    memcpy(boot_record + 7, "EL TORITO SPECIFICATION", 23);
    put_le32(boot_record + 0x47, 18u);              /* каталог в секторе 18 */

    uint8_t* catalog = image + 18u * 2048u;
    catalog[0] = 0x01;
    catalog[1] = 0x00;
    catalog[30] = 0x55;
    catalog[31] = 0xAA;
    uint8_t* entry = catalog + 32;
    entry[0] = 0x88;                                /* загружаемая запись */
    entry[1] = 0x00;                                /* без эмуляции */
    put_le32(entry + 8, 19u);                       /* LBA загрузчика */
    entry[12] = 4;                                  /* 4 x 512 = 2048 байт */

    memcpy(image + 19u * 2048u, boot_code, sizeof(boot_code));

    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        free(image);
        return 0;
    }
    const size_t written = fwrite(image, 1u, size, file);
    fclose(file);
    free(image);
    return written == size;
}

static int write_img(const char* path) {
    uint8_t image[16u * 512u];
    memset(image, 0, sizeof(image));
    memcpy(image, boot_code, sizeof(boot_code));
    image[510] = 0x55;
    image[511] = 0xAA;
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    const size_t written = fwrite(image, 1u, sizeof(image), file);
    fclose(file);
    return written == sizeof(image);
}

/* ---------------- Прогон машины ---------------- */

static void collect_screen(PcMachine* machine, char* out, size_t capacity) {
    out[0] = '\0';
    size_t used = 0;
    for (int row = 0; row < OPENXISO_SCREEN_ROWS; ++row) {
        char line[256];
        if (openxiso_machine_get_screen_line(machine, row, line, sizeof(line)) < 0) {
            break;
        }
        const size_t length = strlen(line);
        if (used + length + 2u >= capacity) {
            break;
        }
        memcpy(out + used, line, length);
        used += length;
        out[used++] = '\n';
        out[used] = '\0';
    }
}

/** Возвращает машину и отдаёт владельцу открытый fd через out_fd. */
static PcMachine* boot_machine(const char* path, int* out_fd) {
    *out_fd = -1;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    PcMachine* machine = openxiso_machine_create();
    if (machine == NULL) {
        close(fd);
        return NULL;
    }
    *out_fd = fd;
    const long long size = lseek(fd, 0, SEEK_END);
    if (openxiso_machine_load_image(machine, fd, size) != 0 ||
        openxiso_machine_start(machine) != 0) {
        return machine;
    }
    for (int slice = 0; slice < 200; ++slice) {
        if (!openxiso_machine_run_slice(machine, 20000u)) {
            break;
        }
    }
    return machine;
}

int main(void) {
    char screen[8192];

    /* --- ISO 9660 + El Torito: POST, загрузчик, INT 10h, INT 13h, HLT --- */
    build_boot_program("OpenXiso boot sector: ", 1);
    const char* iso_path = "/tmp/openxiso_el_torito.iso";
    if (!write_iso(iso_path)) {
        CHECK(0, "не удалось собрать ISO с El Torito");
    } else {
        int image_fd = -1;
        PcMachine* machine = boot_machine(iso_path, &image_fd);
        CHECK(machine != NULL, "машина создана");
        if (machine != NULL) {
            collect_screen(machine, screen, sizeof(screen));
            CHECK(openxiso_machine_status(machine) == OPENXISO_MACHINE_HALTED,
                  "загрузчик завершился инструкцией HLT");
            CHECK(openxiso_machine_instructions(machine) > 40u,
                  "ЦПУ исполнил реальные инструкции загрузчика");
            CHECK(strstr(screen, "OpenXiso Little Legacy BIOS") != NULL,
                  "на экране баннер собственного Legacy BIOS");
            CHECK(strstr(screen, "OpenXiso boot sector") != NULL,
                  "загрузчик напечатал строку через INT 10h");
            CHECK(strstr(screen, "El Torito read OK") != NULL,
                  "INT 13h AH=42h успешно прочитал образ");
            CHECK(strstr(openxiso_machine_log(machine), "POST") != NULL,
                  "журнал POST на русском языке");
            CHECK(strstr(openxiso_machine_log(machine), "El Torito") != NULL,
                  "журнал подтверждает загрузку El Torito");
            CHECK(strstr(openxiso_machine_error(machine), "Неподдерживаемая") == NULL,
                  "неизвестных опкодов нет");
            printf("Состояние: %d, инструкций: %llu\n",
                   (int)openxiso_machine_status(machine),
                   (unsigned long long)openxiso_machine_instructions(machine));
            printf("Экран BIOS:\n%s", screen);
            openxiso_machine_destroy(machine);
        }
        if (image_fd >= 0) {
            close(image_fd);
        }
        remove(iso_path);
    }

    /* --- Сырой .img с MBR --- */
    build_boot_program("MBR boot OK\r\n", 0);
    const char* img_path = "/tmp/openxiso_mbr.img";
    if (!write_img(img_path)) {
        CHECK(0, "не удалось собрать MBR-образ");
    } else {
        int image_fd = -1;
        PcMachine* machine = boot_machine(img_path, &image_fd);
        CHECK(machine != NULL, "машина создана для MBR");
        if (machine != NULL) {
            collect_screen(machine, screen, sizeof(screen));
            CHECK(openxiso_machine_status(machine) == OPENXISO_MACHINE_HALTED,
                  "MBR-загрузчик завершился HLT");
            CHECK(strstr(screen, "MBR boot OK") != NULL,
                  "MBR-загрузчик вывел строку через INT 10h");
            CHECK(strstr(openxiso_machine_log(machine), "MBR") != NULL,
                  "журнал содержит ветку загрузки MBR");
            openxiso_machine_destroy(machine);
        }
        if (image_fd >= 0) {
            close(image_fd);
        }
        remove(img_path);
    }

    /* --- INT 16h: ожидание клавиши и продолжение с того же INT --- */
    build_keyboard_program();
    const char* key_path = "/tmp/openxiso_keyboard.img";
    if (!write_img(key_path)) {
        CHECK(0, "не удалось собрать образ для проверки INT 16h");
    } else {
        int image_fd = -1;
        PcMachine* machine = boot_machine(key_path, &image_fd);
        CHECK(machine != NULL, "машина создана для INT 16h");
        if (machine != NULL) {
            CHECK(openxiso_machine_status(machine) == OPENXISO_MACHINE_WAITING,
                  "пустая очередь INT 16h переводит машину в ожидание");
            const uint64_t before = openxiso_machine_instructions(machine);
            openxiso_machine_push_key(machine, (0x1051u));  /* AH=10h, AL='Q' */
            for (int slice = 0; slice < 20; ++slice) {
                if (!openxiso_machine_run_slice(machine, 20000u)) {
                    break;
                }
            }
            collect_screen(machine, screen, sizeof(screen));
            CHECK(openxiso_machine_status(machine) == OPENXISO_MACHINE_HALTED,
                  "после клавиши загрузчик дошёл до HLT");
            CHECK(strchr(screen, 'Q') != NULL,
                  "символ из очереди напечатан через INT 10h");
            CHECK(openxiso_machine_instructions(machine) > before,
                  "исполнение продолжилось с инструкции INT 16h");
            openxiso_machine_destroy(machine);
        }
        if (image_fd >= 0) {
            close(image_fd);
        }
        remove(key_path);
    }

    printf("Проверок: %d, ошибок: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
