/*
 * test_wide.c — проверка настоящих 32/64-битных режимов интерпретатора.
 *
 * Собирает загрузочный сектор, который РЕАЛЬНО переключает CPU в защищённый
 * режим i386 или long mode x86-64 (LGDT, CR0, EFER, far jmp по дескриптору
 * GDT), исполняет 32/64-битные инструкции с префиксами 0x66 и REX, пишет
 * маркер в VGA-память и останавливается по HLT.
 *
 * Третий тест доказывает, что выбор «16 бит» в интерфейсе не просто подпись:
 * тот же 32-битный загрузчик в 16-битном профиле честно получает отказ CPU.
 */

#include "legacy_bios.h"
#include "pc_machine.h"
#include "x86_cpu.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int checks = 0;
static int failures = 0;

#define CHECK(condition, message)                                            \
    do {                                                                     \
        ++checks;                                                            \
        if (!(condition)) {                                                   \
            ++failures;                                                       \
            printf("ОШИБКА: %s (строка %d)\n", (message), __LINE__);          \
        }                                                                     \
    } while (0)

static uint8_t sector[512];

static void emit(int* position, uint8_t byte) {
    if (*position < 510) {
        sector[(*position)++] = byte;
    }
}

static void emit16(int* position, uint16_t value) {
    emit(position, (uint8_t)(value & 0xFFu));
    emit(position, (uint8_t)(value >> 8));
}

static void emit32(int* position, uint32_t value) {
    emit16(position, (uint16_t)(value & 0xFFFFu));
    emit16(position, (uint16_t)(value >> 16));
}

static void emit64(int* position, uint64_t value) {
    emit32(position, (uint32_t)(value & 0xFFFFFFFFu));
    emit32(position, (uint32_t)(value >> 32));
}

/* Смещения внутри загрузочного сектора и их абсолютные адреса в 0000:7C00. */
#define LOAD_BASE 0x7C00u
#define GDT_OFFSET 0x100u
#define GDTR_OFFSET 0x120u
#define ENTRY_OFFSET 0x80u
#define GDT_ADDRESS_ABS (LOAD_BASE + GDT_OFFSET)
#define GDTR_ADDRESS_ABS (LOAD_BASE + GDTR_OFFSET)
#define ENTRY_ADDRESS_ABS (LOAD_BASE + ENTRY_OFFSET)

/** Заполняет GDT: селектор 08h — 32-битный код, 10h — 64-битный код. */
static void write_gdt(void) {
    memset(sector + GDT_OFFSET, 0, 32);
    const uint8_t code32[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xCF, 0x00 };
    const uint8_t code64[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xAF, 0x00 };
    memcpy(sector + GDT_OFFSET + 8, code32, 8);
    memcpy(sector + GDT_OFFSET + 16, code64, 8);
    sector[GDTR_OFFSET] = 0x17;                  /* предел GDT: 3 записи */
    sector[GDTR_OFFSET + 1] = 0x00;
    const uint32_t base = GDT_ADDRESS_ABS;
    sector[GDTR_OFFSET + 2] = (uint8_t)(base & 0xFFu);
    sector[GDTR_OFFSET + 3] = (uint8_t)((base >> 8) & 0xFFu);
    sector[GDTR_OFFSET + 4] = (uint8_t)((base >> 16) & 0xFFu);
    sector[GDTR_OFFSET + 5] = (uint8_t)((base >> 24) & 0xFFu);
}

/** Общая реальная стадия: LGDT и включение PE в CR0. */
static void emit_real_prefix(int* position, int long_mode) {
    emit(position, 0x66); emit(position, 0x0F); emit(position, 0x01);
    emit(position, 0x16);                       /* LGDT [disp16], /2 */
    emit16(position, (uint16_t)GDTR_ADDRESS_ABS);

    if (long_mode) {
        emit(position, 0x66); emit(position, 0xB9);
        emit32(position, 0xC0000080u);          /* MOV ECX, MSR_EFER */
        emit(position, 0x66); emit(position, 0xB8);
        emit32(position, 0x00000100u);          /* MOV EAX, LME      */
        emit(position, 0x66); emit(position, 0x31); emit(position, 0xD2);
        emit(position, 0x0F); emit(position, 0x30);   /* WRMSR       */
    }

    emit(position, 0x0F); emit(position, 0x20); emit(position, 0xC0);
    if (long_mode) {
        emit(position, 0x66); emit(position, 0x0D);
        emit32(position, 0x80000001u);               /* OR EAX, PE|PG */
    } else {
        emit(position, 0x66); emit(position, 0x83);
        emit(position, 0xC8); emit(position, 0x01);  /* OR EAX, 1   */
    }
    emit(position, 0x0F); emit(position, 0x22); emit(position, 0xC0);
}

static void build_32bit_kernel(void) {
    memset(sector, 0, sizeof(sector));
    int position = 0;
    write_gdt();
    emit_real_prefix(&position, 0);
    /* JMP FAR 08h:ENTRY_ADDRESS (префикс 0x66 даёт 32-битное смещение). */
    emit(&position, 0x66); emit(&position, 0xEA);
    emit32(&position, ENTRY_ADDRESS_ABS);
    emit16(&position, 0x0008u);

    position = ENTRY_OFFSET;
    emit(&position, 0xC7); emit(&position, 0x05);   /* MOV dword [disp32], imm32 */
    emit32(&position, 0x000B8000u);
    emit32(&position, 0x07320733u);                 /* '3', attr, '2', attr */
    emit(&position, 0xB8);
    emit32(&position, 0x12345678u);                 /* MOV EAX, 0x12345678 */
    emit(&position, 0x05);
    emit32(&position, 1u);                          /* ADD EAX, 1          */
    emit(&position, 0xF4);                          /* HLT                 */

    sector[510] = 0x55;
    sector[511] = 0xAA;
}

static void build_64bit_kernel(void) {
    memset(sector, 0, sizeof(sector));
    int position = 0;
    write_gdt();
    emit_real_prefix(&position, 1);
    /* CS пока 16-битный: 0x66 даёт 32-битное смещение far jmp. */
    emit(&position, 0x66); emit(&position, 0xEA);
    emit32(&position, ENTRY_ADDRESS_ABS);
    emit16(&position, 0x0010u);                     /* селектор с L=1 */

    position = ENTRY_OFFSET;
    emit(&position, 0x48); emit(&position, 0xB8);   /* REX.W: MOVABS RAX, 0xB8000 */
    emit64(&position, 0x000B8000u);
    emit(&position, 0xC7); emit(&position, 0x00);   /* MOV dword [RAX], "64" */
    emit32(&position, 0x07340736u);                 /* '6', attr, '4', attr */
    emit(&position, 0x49); emit(&position, 0xB8);   /* REX.W+B: MOVABS R8, 42 */
    emit64(&position, 42u);
    emit(&position, 0x49); emit(&position, 0x83);   /* REX.W+B: ADD R8, 1 */
    emit(&position, 0xC0); emit(&position, 0x01);
    emit(&position, 0x48); emit(&position, 0xB8);   /* MOVABS RAX, 0x12345678 */
    emit64(&position, 0x12345678u);
    emit(&position, 0x48); emit(&position, 0x83);   /* REX.W: ADD RAX, 1 */
    emit(&position, 0xC0); emit(&position, 0x01);
    emit(&position, 0xF4);                          /* HLT */

    sector[510] = 0x55;
    sector[511] = 0xAA;
}

static int write_image(const char* path) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    const size_t written = fwrite(sector, 1u, sizeof(sector), file);
    fclose(file);
    return written == sizeof(sector);
}

static PcMachine* run_kernel(const char* path, int mode, int* out_fd) {
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
    if (openxiso_machine_set_cpu_mode(machine, mode) != 0 ||
        openxiso_machine_load_image(machine, fd, (long long)sizeof(sector)) != 0 ||
        openxiso_machine_start(machine) != 0) {
        return machine;
    }
    for (int slice = 0; slice < 100; ++slice) {
        if (!openxiso_machine_run_slice(machine, 20000u)) {
            break;
        }
    }
    return machine;
}

static void screen_text(PcMachine* machine, char* out, size_t capacity) {
    out[0] = '\0';
    size_t used = 0;
    for (int row = 0; row < 4; ++row) {
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

int main(void) {
    char screen[1024];
    const char* path = "/tmp/openxiso_wide.img";

    /* --- 32-битный защищённый режим i386 --- */
    build_32bit_kernel();
    if (!write_image(path)) {
        CHECK(0, "не удалось записать 32-битный образ");
    } else {
        int fd = -1;
        PcMachine* machine = run_kernel(path, 1, &fd);
        CHECK(machine != NULL, "машина создана для 32 бит");
        if (machine != NULL) {
            screen_text(machine, screen, sizeof(screen));
            CHECK(x86_get_mode(&machine->cpu) == X86_MODE_32,
                  "CPU перешёл в защищённый режим i386");
            CHECK((machine->cpu.cr0 & X86_CR0_PE) != 0,
                  "CR0.PE установлен гостем");
            CHECK(openxiso_machine_status(machine) == OPENXISO_MACHINE_HALTED,
                  "32-битное ядро завершилось по HLT без ошибки");
            CHECK(machine->cpu.gpr[X86_RAX] == 0x12345679u,
                  "32-битный ADD EAX,1 исполнен на настоящих регистрах");
            CHECK(strstr(screen, "32") != NULL,
                  "32-битный MOV записал маркер прямо в VGA-память");
            CHECK(strstr(openxiso_machine_log(machine), "32 бита") != NULL,
                  "журнал POST сообщает выбранный профиль CPU");
            printf("32 бита: режим=%d, RAX=%llX, инструкций=%llu\n",
                   (int)x86_get_mode(&machine->cpu),
                   (unsigned long long)machine->cpu.gpr[X86_RAX],
                   (unsigned long long)openxiso_machine_instructions(machine));
            openxiso_machine_destroy(machine);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    /* --- 64-битный long mode x86-64 с REX-префиксами --- */
    build_64bit_kernel();
    if (!write_image(path)) {
        CHECK(0, "не удалось записать 64-битный образ");
    } else {
        int fd = -1;
        PcMachine* machine = run_kernel(path, 2, &fd);
        CHECK(machine != NULL, "машина создана для 64 бит");
        if (machine != NULL) {
            screen_text(machine, screen, sizeof(screen));
            CHECK(x86_get_mode(&machine->cpu) == X86_MODE_64,
                  "CPU перешёл в long mode x86-64");
            CHECK((machine->cpu.efer & X86_EFER_LME) != 0,
                  "EFER.LME установлен через WRMSR");
            CHECK(openxiso_machine_status(machine) == OPENXISO_MACHINE_HALTED,
                  "64-битное ядро завершилось по HLT без ошибки");
            CHECK(machine->cpu.gpr[X86_RAX] == 0x12345679u,
                  "REX.W: 64-битный ADD RAX,1 исполнен");
            CHECK(machine->cpu.gpr[X86_R8] == 43u,
                  "REX.B: регистр R8 доступен и ADD R8,1 исполнен");
            CHECK(strstr(screen, "64") != NULL,
                  "64-битный MOV записал маркер в VGA-память");
            CHECK(machine->memory_size == OPENXISO_WIDE_MEMORY_SIZE,
                  "для широкого режима выделено 64 МБ памяти");
            printf("64 бита: режим=%d, RAX=%llX, R8=%llu, инструкций=%llu\n",
                   (int)x86_get_mode(&machine->cpu),
                   (unsigned long long)machine->cpu.gpr[X86_RAX],
                   (unsigned long long)machine->cpu.gpr[X86_R8],
                   (unsigned long long)openxiso_machine_instructions(machine));
            openxiso_machine_destroy(machine);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    /* --- Профиль 16 бит обязан запретить переход --- */
    build_32bit_kernel();
    if (!write_image(path)) {
        CHECK(0, "не удалось записать образ для проверки ограничения");
    } else {
        int fd = -1;
        PcMachine* machine = run_kernel(path, 0, &fd);
        CHECK(machine != NULL, "машина создана для 16 бит");
        if (machine != NULL) {
            CHECK(x86_get_max_mode(&machine->cpu) == X86_MODE_16,
                  "профиль 16 бит действительно выбран");
            CHECK(openxiso_machine_status(machine) == OPENXISO_MACHINE_ERROR,
                  "32-битная инструкция в 16-битном профиле остановлена");
            CHECK(strstr(openxiso_machine_error(machine), "32-битный процессор") != NULL,
                  "диагностика на русском объясняет причину отказа");
            CHECK(machine->memory_size == OPENXISO_MEMORY_SIZE,
                  "в 16-битном профиле остаётся 1 МБ памяти");
            openxiso_machine_destroy(machine);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    remove(path);
    printf("Проверок: %d, ошибок: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
