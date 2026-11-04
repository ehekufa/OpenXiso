#ifndef OPENXISO_PC_MACHINE_H
#define OPENXISO_PC_MACHINE_H

/*
 * pc_machine.h — виртуальный PC: 1 МБ ОЗУ, VGA-текст 80x25, дисковый образ,
 * клавиатура и собственный Legacy BIOS. Открытый fd образа принадлежит Java.
 */

#include <stdint.h>

#include "boot_image.h"
#include "x86_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPENXISO_MEMORY_SIZE (1024u * 1024u)
#define OPENXISO_VGA_ADDRESS 0xB8000u
#define OPENXISO_BIOS_ROM_ADDRESS 0xF0000u
#define OPENXISO_BOOT_ADDRESS 0x7C00u
#define OPENXISO_SCREEN_COLUMNS 80
#define OPENXISO_SCREEN_ROWS 25
#define OPENXISO_FIRMWARE_PORT 0x0510u
#define OPENXISO_FIRMWARE_POST_PORT 0x0511u
#define OPENXISO_LOG_CAPACITY 16

typedef enum PcMachineStatus {
    OPENXISO_MACHINE_IDLE = 0,
    OPENXISO_MACHINE_RUNNING = 1,
    OPENXISO_MACHINE_HALTED = 2,
    OPENXISO_MACHINE_ERROR = 3,
    OPENXISO_MACHINE_WAITING = 4
} PcMachineStatus;

typedef struct PcMachine {
    X86Cpu cpu;
    uint8_t* memory;

    int fd;                    /* не закрывается здесь: владелец — Java/SAF */
    long long image_size;
    OpenXisoBootPlan plan;
    PcMachineStatus status;

    uint16_t keys[64];
    int key_head;
    int key_tail;

    uint8_t cursor_row;
    uint8_t cursor_col;
    uint32_t ticks;

    char serial_line[192];
    int serial_length;

    char log_lines[OPENXISO_LOG_CAPACITY][192];
    int log_count;
    char log_last[192];      /* базовая строка для свёртки повторов */
    int log_repeat;
    char log_snapshot[OPENXISO_LOG_CAPACITY * 200];
    char error[192];

    uint8_t transfer_buffer[65536];
} PcMachine;

PcMachine* openxiso_machine_create(void);
void openxiso_machine_destroy(PcMachine* machine);

/** Принимает fd образа, открытый ContentResolver, и строит план загрузки. */
int openxiso_machine_load_image(PcMachine* machine, int fd, long long size);

/** POST + INT 19h: переводит машину в состояние исполнения загрузчика. */
int openxiso_machine_start(PcMachine* machine);

/** Исполняет не более max_instructions инструкций. Возвращает 1, если можно продолжать. */
int openxiso_machine_run_slice(PcMachine* machine, uint32_t max_instructions);

/** Строка экрана в UTF-8 (VGA хранит CP866). */
int openxiso_machine_get_screen_line(const PcMachine* machine, int row,
                                     char* out, size_t capacity);

PcMachineStatus openxiso_machine_status(const PcMachine* machine);
const char* openxiso_machine_log(const PcMachine* machine);
const char* openxiso_machine_error(const PcMachine* machine);
void openxiso_machine_push_key(PcMachine* machine, uint16_t scan_and_ascii);
void openxiso_machine_stop(PcMachine* machine);
uint64_t openxiso_machine_instructions(const PcMachine* machine);

#ifdef __cplusplus
}
#endif

#endif /* OPENXISO_PC_MACHINE_H */
