/*
 * pc_machine.c — сборка виртуального PC и цикл исполнения.
 *
 * Java открывает образ через SAF и передаёт сюда файловый дескриптор; ядро
 * читает его через pread по запросам INT 13h и не копирует весь ISO в память.
 * Порты 0x510/0x511 — «фирменные» вызовы собственного Legacy BIOS.
 */

#include "include/pc_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/legacy_bios.h"

/* ---------------- Кодировка экрана ---------------- */

static uint32_t cp866_to_unicode(uint8_t byte) {
    if (byte < 0x80u) {
        return byte;
    }
    if (byte <= 0x9Fu) {
        return 0x0410u + (byte - 0x80u);   /* А..Я */
    }
    if (byte <= 0xAFu) {
        return 0x0430u + (byte - 0xA0u);   /* а..п */
    }
    if (byte >= 0xE0u && byte <= 0xEFu) {
        return 0x0440u + (byte - 0xE0u);   /* р..я */
    }
    if (byte == 0xF0u) {
        return 0x0401u;                     /* Ё */
    }
    if (byte == 0xF1u) {
        return 0x0451u;                     /* ё */
    }
    return (uint32_t)'?';
}

static int append_utf8(char* out, size_t capacity, size_t* position, uint32_t point) {
    if (point < 0x80u) {
        if (*position + 1u >= capacity) return 0;
        out[(*position)++] = (char)point;
    } else if (point < 0x800u) {
        if (*position + 2u >= capacity) return 0;
        out[(*position)++] = (char)(0xC0u | (point >> 6));
        out[(*position)++] = (char)(0x80u | (point & 0x3Fu));
    } else {
        if (*position + 3u >= capacity) return 0;
        out[(*position)++] = (char)(0xE0u | (point >> 12));
        out[(*position)++] = (char)(0x80u | ((point >> 6) & 0x3Fu));
        out[(*position)++] = (char)(0x80u | (point & 0x3Fu));
    }
    return 1;
}

/* ---------------- Порты «железа» ---------------- */

static int machine_interrupt_hook(void* context, uint8_t vector) {
    PcMachine* machine = (PcMachine*)context;
    legacy_bios_interrupt(machine, vector);
    return 1;
}

static uint8_t machine_port_in(void* context, uint16_t port) {
    PcMachine* machine = (PcMachine*)context;
    switch (port) {
        case 0x0060u:
            if (machine->key_head != machine->key_tail) {
                const uint16_t key = machine->keys[machine->key_head];
                machine->key_head = (machine->key_head + 1) % 64;
                return (uint8_t)(key & 0xFFu);
            }
            return 0x00u;
        case 0x0064u:
            return machine->key_head != machine->key_tail ? 0x01u : 0x00u;
        case 0x03FDu:
            return 0x20u;                                  /* UART готов к передаче */
        case OPENXISO_FIRMWARE_PORT:
            return 0x10u;                                  /* версия прошивки */
        case OPENXISO_FIRMWARE_POST_PORT:
            return 0x01u;
        default:
            return 0xFFu;
    }
}

static void machine_port_out(void* context, uint16_t port, uint8_t value) {
    PcMachine* machine = (PcMachine*)context;
    switch (port) {
        case OPENXISO_FIRMWARE_PORT:
            legacy_bios_interrupt(machine, value);
            break;
        case OPENXISO_FIRMWARE_POST_PORT:
            legacy_bios_post(machine);
            break;
        case 0x03F8u:                                      /* COM1: отладочный вывод */
            if (value == '\n' || value == '\r') {
                if (machine->serial_length > 0) {
                    machine->serial_line[machine->serial_length] = '\0';
                    legacy_bios_log(machine, "COM1: %s", machine->serial_line);
                    machine->serial_length = 0;
                }
            } else if (machine->serial_length + 1 < (int)sizeof(machine->serial_line)) {
                machine->serial_line[machine->serial_length++] = (char)value;
            }
            break;
        case 0x0CF9u:                                      /* сброс/выключение */
            if ((value & 0x06u) != 0) {
                machine->cpu.halted = 1;
                machine->status = OPENXISO_MACHINE_HALTED;
                legacy_bios_log(machine, "Получен сигнал выключения питания");
            }
            break;
        default:
            break;
    }
}

/* ---------------- Жизненный цикл машины ---------------- */

PcMachine* openxiso_machine_create(void) {
    PcMachine* machine = (PcMachine*)calloc(1, sizeof(PcMachine));
    if (machine == NULL) {
        return NULL;
    }
    machine->memory = (uint8_t*)calloc(OPENXISO_MEMORY_SIZE, 1u);
    if (machine->memory == NULL) {
        free(machine);
        return NULL;
    }
    machine->fd = -1;
    machine->status = OPENXISO_MACHINE_IDLE;
    machine->cpu.memory = machine->memory;
    machine->cpu.memory_size = OPENXISO_MEMORY_SIZE;
    machine->cpu.context = machine;
    machine->cpu.port_in = machine_port_in;
    machine->cpu.port_out = machine_port_out;
    machine->cpu.interrupt_hook = machine_interrupt_hook;
    return machine;
}

void openxiso_machine_destroy(PcMachine* machine) {
    if (machine == NULL) {
        return;
    }
    /* fd не закрываем: он принадлежит ParcelFileDescriptor в Java. */
    free(machine->memory);
    free(machine);
}

int openxiso_machine_load_image(PcMachine* machine, int fd, long long size) {
    if (machine == NULL) {
        return -1;
    }
    machine->fd = fd;
    machine->image_size = size;
    if (openxiso_boot_plan(fd, size, &machine->plan,
                           machine->error, sizeof(machine->error)) != 0) {
        legacy_bios_log(machine, "Образ не принят: %s", machine->error);
        machine->status = OPENXISO_MACHINE_ERROR;
        return -1;
    }
    if (machine->plan.el_torito) {
        legacy_bios_log(machine,
                        "Образ ISO 9660 / El Torito: LBA %u, секторов %u",
                        machine->plan.boot_lba, machine->plan.boot_sectors);
    } else {
        legacy_bios_log(machine, "Образ диска с MBR: загрузочный сектор LBA 0");
    }
    return 0;
}

int openxiso_machine_start(PcMachine* machine) {
    if (machine == NULL || machine->fd < 0) {
        return -1;
    }
    memset(machine->memory, 0, OPENXISO_MEMORY_SIZE);
    x86_reset(&machine->cpu, machine->memory, OPENXISO_MEMORY_SIZE);
    machine->cpu.context = machine;
    machine->cpu.port_in = machine_port_in;
    machine->cpu.port_out = machine_port_out;
    machine->cpu.interrupt_hook = machine_interrupt_hook;
    machine->cpu.flags = 0x0002u;

    machine->cpu.sregs[X86_CS] = 0xF000u;
    machine->cpu.ip = 0xFFF0u;
    machine->cpu.sregs[X86_SS] = 0x0000u;
    machine->cpu.regs[X86_SP] = 0x7BFEu;
    machine->cpu.sregs[X86_DS] = 0x0000u;
    machine->cpu.sregs[X86_ES] = 0x0000u;

    machine->key_head = 0;
    machine->key_tail = 0;
    machine->cursor_row = 0;
    machine->cursor_col = 0;
    machine->ticks = 0;
    machine->error[0] = '\0';

    legacy_bios_install(machine);
    machine->status = OPENXISO_MACHINE_RUNNING;
    legacy_bios_log(machine, "Машина запущена: CPU 8086, вектор сброса F000:FFF0");
    return 0;
}

int openxiso_machine_run_slice(PcMachine* machine, uint32_t max_instructions) {
    if (machine == NULL || machine->status == OPENXISO_MACHINE_ERROR) {
        return 0;
    }
    if (machine->cpu.waiting) {
        machine->status = OPENXISO_MACHINE_WAITING;
        return 0;
    }
    if (machine->cpu.halted) {
        machine->status = OPENXISO_MACHINE_HALTED;
        return 0;
    }

    for (uint32_t index = 0; index < max_instructions; ++index) {
        if (!x86_step(&machine->cpu)) {
            break;
        }
        if ((machine->cpu.instructions & 0x3FFFu) == 0u) {
            machine->ticks += 1u;
        }
    }

    if (machine->cpu.fault) {
        snprintf(machine->error, sizeof(machine->error),
                 "Неподдерживаемая инструкция %02Xh по адресу %04X:%04X",
                 machine->cpu.fault_opcode,
                 machine->cpu.sregs[X86_CS], machine->cpu.ip);
        legacy_bios_log(machine, "Ошибка ЦПУ: %s", machine->error);
        machine->status = OPENXISO_MACHINE_ERROR;
        return 0;
    }
    if (machine->cpu.waiting) {
        machine->status = OPENXISO_MACHINE_WAITING;
        return 0;
    }
    if (machine->cpu.halted) {
        machine->status = OPENXISO_MACHINE_HALTED;
        legacy_bios_log(machine, "Загрузчик выполнил HLT, машина остановлена");
        return 0;
    }
    return 1;
}

int openxiso_machine_get_screen_line(const PcMachine* machine, int row,
                                     char* out, size_t capacity) {
    if (machine == NULL || out == NULL || capacity == 0 ||
        row < 0 || row >= OPENXISO_SCREEN_ROWS) {
        return -1;
    }
    size_t position = 0;
    size_t last_non_space = 0;
    for (int column = 0; column < OPENXISO_SCREEN_COLUMNS; ++column) {
        const size_t index = OPENXISO_VGA_ADDRESS
                             + ((size_t)row * OPENXISO_SCREEN_COLUMNS + column) * 2u;
        const uint8_t character = machine->memory[index];
        const uint8_t raw = character == 0 ? (uint8_t)' ' : character;
        if (!append_utf8(out, capacity, &position, cp866_to_unicode(raw))) {
            break;
        }
        if (raw != ' ') {
            last_non_space = position;
        }
    }
    out[last_non_space] = '\0';
    return (int)last_non_space;
}

PcMachineStatus openxiso_machine_status(const PcMachine* machine) {
    return machine == NULL ? OPENXISO_MACHINE_ERROR : machine->status;
}

const char* openxiso_machine_log(const PcMachine* machine) {
    if (machine == NULL) {
        return "";
    }
    PcMachine* writable = (PcMachine*)machine;
    size_t position = 0;
    writable->log_snapshot[0] = '\0';
    for (int index = 0; index < machine->log_count; ++index) {
        const size_t remaining = sizeof(writable->log_snapshot) - position;
        if (remaining <= 1u) {
            break;
        }
        const int written = snprintf(writable->log_snapshot + position, remaining,
                                     "%s%s", index == 0 ? "" : "\n",
                                     machine->log_lines[index]);
        if (written < 0 || (size_t)written >= remaining) {
            break;
        }
        position += (size_t)written;
    }
    return writable->log_snapshot;
}

const char* openxiso_machine_error(const PcMachine* machine) {
    return machine == NULL ? "Машина не создана" : machine->error;
}

void openxiso_machine_push_key(PcMachine* machine, uint16_t scan_and_ascii) {
    if (machine == NULL) {
        return;
    }
    const int next = (machine->key_tail + 1) % 64;
    if (next != machine->key_head) {
        machine->keys[machine->key_tail] = scan_and_ascii;
        machine->key_tail = next;
    }
    if (machine->cpu.waiting) {
        machine->cpu.waiting = 0;
        machine->status = OPENXISO_MACHINE_RUNNING;
    }
}

void openxiso_machine_stop(PcMachine* machine) {
    if (machine == NULL) {
        return;
    }
    machine->cpu.halted = 1;
    machine->cpu.waiting = 0;
    machine->status = OPENXISO_MACHINE_HALTED;
    legacy_bios_log(machine, "Эмуляция остановлена пользователем");
}

uint64_t openxiso_machine_instructions(const PcMachine* machine) {
    return machine == NULL ? 0u : machine->cpu.instructions;
}
