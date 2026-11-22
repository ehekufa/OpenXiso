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
    /* The guest may replace any IVT entry; do not intercept its handler. */
    uint32_t entry = (uint32_t)vector * 4u;
    if (x86_read_word(&machine->cpu, entry) != (uint16_t)(0x1000u + vector * 16u) ||
        x86_read_word(&machine->cpu, entry + 2u) != 0xF000u) return 0;
    legacy_bios_interrupt(machine, vector);
    return 1;
}

/* Some boot loaders invoke BIOS vectors with PUSHF + far CALL instead of
 * INT. Intercept at the ROM entry before the compatibility stub overwrites
 * AX/DX, and return through the caller's interrupt-compatible stack frame. */
static int machine_firmware_entry(PcMachine* machine) {
    X86Cpu* cpu = &machine->cpu;
    if ((cpu->cr0 & X86_CR0_PE) || cpu->sregs[X86_CS] != 0xf000 ||
        cpu->ip < 0x1000 || cpu->ip >= 0x2000 || ((cpu->ip - 0x1000) & 15u)) return 0;
    uint8_t vector = (uint8_t)((cpu->ip - 0x1000) / 16);
    legacy_bios_interrupt(machine, vector);
    ++cpu->instructions;
    if (cpu->waiting || cpu->halted || cpu->fault || cpu->triple_fault ||
        cpu->sregs[X86_CS] != 0xf000) return 1;
    const uint32_t result_flags = cpu->flags;
    const uint16_t ip = x86_pop(cpu);
    const uint16_t cs = x86_pop(cpu);
    const uint16_t saved_flags = x86_pop(cpu);
    const uint32_t returned_flags = (saved_flags & ~(X86_CF | X86_ZF)) |
                                    (result_flags & (X86_CF | X86_ZF)) | 2u;
    cpu->flags = (cpu->flags & 0xffff0000u) | returned_flags;
    cpu->ip = ip;
    x86_load_segment(cpu, X86_CS, cs);
    return 1;
}

static uint8_t machine_port_in(void* context, uint16_t port) {
    PcMachine* machine = (PcMachine*)context;
    return devices_port_in(machine, port);
}
static uint64_t machine_mmio_read(void* ctx, uint64_t addr, int width){ return devices_mmio_read((PcMachine*)ctx, addr, width); }
static void machine_mmio_write(void* ctx, uint64_t addr, int width, uint64_t v){ devices_mmio_write((PcMachine*)ctx, addr, width, v); }
static int machine_mmio_check(void* ctx, uint64_t addr){ return devices_mmio_check((PcMachine*)ctx, addr); }
static int machine_irq_check(void* ctx){ return devices_irq_check((PcMachine*)ctx); }

static void machine_port_out(void* context, uint16_t port, uint8_t value) {
    PcMachine* machine = (PcMachine*)context;
    devices_port_out(machine, port, value);
}

/* ---------------- Жизненный цикл машины ---------------- */

PcMachine* openxiso_machine_create(void) {
    PcMachine* machine = (PcMachine*)calloc(1, sizeof(PcMachine));
    if (machine == NULL) {
        return NULL;
    }
    machine->memory_size = OPENXISO_MEMORY_SIZE;
    machine->memory = (uint8_t*)calloc(machine->memory_size, 1u);
    if (machine->memory == NULL) {
        free(machine);
        return NULL;
    }
    machine->fd = -1;
    machine->status = OPENXISO_MACHINE_IDLE;
    machine->cpu.memory = machine->memory;
    machine->cpu.memory_size = machine->memory_size;
    machine->cpu.max_mode = X86_MODE_16;
    machine->cpu.context = machine;
    machine->cpu.port_in = machine_port_in;
    machine->cpu.port_out = machine_port_out;
    machine->cpu.mmio_read = machine_mmio_read;
    machine->cpu.mmio_write = machine_mmio_write;
    machine->cpu.mmio_check = machine_mmio_check;
    machine->cpu.irq_check = machine_irq_check;
    machine->cpu.interrupt_hook = machine_interrupt_hook;
    devices_init(machine);
    return machine;
}

void openxiso_machine_destroy(PcMachine* machine) {
    if (machine == NULL) {
        return;
    }
    free(machine->framebuffer);
    free(machine->memory);
    free(machine);
}

int openxiso_machine_set_cpu_mode(PcMachine* machine, int mode) {
    if (machine == NULL) {
        return -1;
    }
    const X86Mode selected = mode == 2 ? X86_MODE_64
                             : mode == 1 ? X86_MODE_32 : X86_MODE_16;
    const size_t wanted = selected == X86_MODE_16
                          ? (size_t)OPENXISO_MEMORY_SIZE
                          : (size_t)OPENXISO_WIDE_MEMORY_SIZE;
    if (wanted != machine->memory_size) {
        uint8_t* replacement = (uint8_t*)calloc(wanted, 1u);
        if (replacement == NULL) {
            snprintf(machine->error, sizeof(machine->error),
                     "Не удалось выделить %zu МБ памяти для гостя",
                     wanted / (1024u * 1024u));
            return -1;
        }
        free(machine->memory);
        machine->memory = replacement;
        machine->memory_size = wanted;
        machine->cpu.memory = replacement;
        machine->cpu.memory_size = wanted;
    }
    machine->cpu_mode = selected;
    x86_set_max_mode(&machine->cpu, selected);
    return 0;
}

X86Mode openxiso_machine_cpu_mode(const PcMachine* machine) {
    return machine == NULL ? X86_MODE_16 : machine->cpu_mode;
}

int openxiso_machine_load_image(PcMachine* machine, int fd, long long size) {
    if (machine == NULL) {
        return -1;
    }
    /* Новая загрузка начинает новый журнал POST. */
    machine->log_count = 0;
    machine->log_repeat = 0;
    machine->log_last[0] = '\0';
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
                        "ISO 9660 / El Torito: LBA %u, режим «%s»",
                        machine->plan.boot_lba,
                        openxiso_emulation_name(machine->plan.emulation));
        legacy_bios_log(machine,
                        "Эмулируемый диск: %llu байт, сектор %u, геометрия %u/%u",
                        (unsigned long long)machine->plan.image_bytes,
                        openxiso_disk_sector_size(&machine->plan),
                        machine->plan.sectors_per_track, machine->plan.heads);
    } else {
        legacy_bios_log(machine, "Образ диска с MBR: загрузочный сектор LBA 0");
    }
    return 0;
}

int openxiso_machine_start(PcMachine* machine) {
    if (machine == NULL || machine->fd < 0) {
        return -1;
    }
    memset(machine->memory, 0, machine->memory_size);
    x86_reset(&machine->cpu, machine->memory, machine->memory_size);
    x86_set_max_mode(&machine->cpu, machine->cpu_mode);
    machine->cpu.context = machine;
    machine->cpu.port_in = machine_port_in;
    machine->cpu.port_out = machine_port_out;
    machine->cpu.mmio_read = machine_mmio_read;
    machine->cpu.mmio_write = machine_mmio_write;
    machine->cpu.mmio_check = machine_mmio_check;
    machine->cpu.irq_check = machine_irq_check;
    machine->cpu.interrupt_hook = machine_interrupt_hook;
    machine->cpu.flags = 0x0002u;
    devices_reset(machine);
    /* Legacy POST programs PIT channel 0 and unmasks its timer IRQ. */
    devices_port_out(machine, 0x43, 0x36);
    devices_port_out(machine, 0x40, 0);
    devices_port_out(machine, 0x40, 0);
    devices_port_out(machine, 0x21, 0xfe);

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
    legacy_bios_log(machine, "Машина запущена: профиль %s (%s), сброс F000:FFF0",
                    x86_mode_name(machine->cpu_mode),
                    x86_mode_description(machine->cpu_mode));
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
        /* HLT with IF set is an interruptible idle state, not power-off.
         * Advance emulated devices even while no instructions execute. */
        if (!(machine->cpu.flags & X86_IF) || machine->status == OPENXISO_MACHINE_HALTED) {
            machine->status = OPENXISO_MACHINE_HALTED;
            return 0;
        }
        devices_tick(machine, max_instructions);
        if (!x86_check_and_deliver_irq(&machine->cpu)) {
            machine->status = OPENXISO_MACHINE_WAITING;
            return 0;
        }
        machine->status = OPENXISO_MACHINE_RUNNING;
    }

    for (uint32_t index = 0; index < max_instructions; ++index) {
        if (!machine_firmware_entry(machine) && !x86_step(&machine->cpu)) {
            break;
        }
        devices_tick(machine, 1);
        if (machine->cpu.waiting || machine->cpu.halted || machine->cpu.fault ||
            machine->cpu.triple_fault) break;
    }
    if (machine->cpu.triple_fault) {
        snprintf(machine->error, sizeof(machine->error), "Тройная ошибка (triple fault): нет обработчика исключения");
        legacy_bios_log(machine, "Ошибка ЦПУ: %s", machine->error);
        machine->status = OPENXISO_MACHINE_ERROR;
        return 0;
    }

    if (machine->cpu.fault) {
        const uint8_t opcode = machine->cpu.fault_opcode;
        const uint64_t address = x86_wide_ip(&machine->cpu);
        if (machine->cpu.fault_reason != NULL) {
            snprintf(machine->error, sizeof(machine->error),
                     "%s: код %02Xh по адресу %llXh",
                     machine->cpu.fault_reason, opcode,
                     (unsigned long long)address);
        } else if (opcode == 0x66u || opcode == 0x67u || opcode == 0x0Fu) {
            snprintf(machine->error, sizeof(machine->error),
                     "Гостю нужен 32-битный защищённый режим (386+): "
                     "код %02Xh по адресу %04X:%04X",
                     opcode, machine->cpu.sregs[X86_CS], machine->cpu.ip);
        } else {
            snprintf(machine->error, sizeof(machine->error),
                     "Неподдерживаемая инструкция %02Xh по адресу %04X:%04X",
                     opcode, machine->cpu.sregs[X86_CS], machine->cpu.ip);
        }
        legacy_bios_log(machine, "Ошибка ЦПУ: %s", machine->error);
        machine->status = OPENXISO_MACHINE_ERROR;
        return 0;
    }
    if (machine->cpu.waiting) {
        machine->status = OPENXISO_MACHINE_WAITING;
        return 0;
    }
    if (machine->cpu.halted) {
        if (machine->cpu.flags & X86_IF) {
            machine->status = OPENXISO_MACHINE_WAITING;
        } else {
            machine->status = OPENXISO_MACHINE_HALTED;
            legacy_bios_log(machine, "Загрузчик выполнил HLT при запрещённых прерываниях");
        }
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
    machine->kbd_output = (uint8_t)(scan_and_ascii >> 8);
    machine->kbd_output_full = 1;
    devices_raise_irq(machine, 1);
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
