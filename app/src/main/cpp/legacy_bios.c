#define _POSIX_C_SOURCE 200809L

/*
 * legacy_bios.c — собственный «маленький Legacy BIOS» OpenXiso.
 *
 * ROM размером 64 КБ отображается по F000:0000. В нём лежат: таблица векторов
 * прерываний (IVT), 256 машинных заглушек `mov al,N; mov dx,510h; out dx,al;
 * iret`, код POST и вектор сброса F000:FFF0. Заглушки остаются настоящими
 * машинными кодами и адресуются из IVT, но рабочий путь INT идёт через
 * firmware-хук CPU: так iret не затирает CF от INT 13h и не портит AL.
 * Порт 0x510 сохранён как совместимый способ вызвать сервис из ROM.
 * Реализованы INT 10h (VGA-текст), 13h (диск El Torito/MBR), 16h (клавиатура),
 * 19h (bootstrap), 12h, 11h, 15h, 1Ah, 08h, 1Ch.
 */

#include "include/legacy_bios.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "include/boot_image.h"

const char* legacy_bios_version(void) {
    return "OpenXiso Little Legacy BIOS 0.1";
}

/* ---------------- Регистры и флаги (короткие помощники) ---------------- */

static uint8_t reg8(const PcMachine* machine, int index) {
    const uint16_t value = machine->cpu.regs[index & 3];
    return index < 4 ? (uint8_t)(value & 0xFFu) : (uint8_t)(value >> 8);
}

static void set_reg8(PcMachine* machine, int index, uint8_t value) {
    uint16_t* target = &machine->cpu.regs[index & 3];
    if (index < 4) {
        *target = (uint16_t)((*target & 0xFF00u) | value);
    } else {
        *target = (uint16_t)((*target & 0x00FFu) | ((uint16_t)value << 8));
    }
}

static void put8(PcMachine* machine, uint16_t segment, uint16_t offset, uint8_t value) {
    x86_write_byte(&machine->cpu, x86_physical(segment, offset), value);
}

static uint16_t mem16(const PcMachine* machine, uint16_t segment, uint16_t offset) {
    return x86_read_word(&machine->cpu, x86_physical(segment, offset));
}

static void put16(PcMachine* machine, uint16_t segment, uint16_t offset, uint16_t value) {
    x86_write_word(&machine->cpu, x86_physical(segment, offset), value);
}

static void carry(PcMachine* machine, int value) {
    x86_set_carry(&machine->cpu, value);
}

/* ---------------- UTF-8 <-> CP866 ---------------- */

static uint32_t next_codepoint(const char** cursor) {
    const unsigned char* text = (const unsigned char*)*cursor;
    if (text[0] == 0) {
        return 0;
    }
    if (text[0] < 0x80u) {
        *cursor += 1;
        return text[0];
    }
    if ((text[0] & 0xE0u) == 0xC0u && text[1] != 0) {
        const uint32_t point = (uint32_t)(((text[0] & 0x1Fu) << 6) | (text[1] & 0x3Fu));
        *cursor += 2;
        return point;
    }
    if ((text[0] & 0xF0u) == 0xE0u && text[1] != 0 && text[2] != 0) {
        const uint32_t point = (uint32_t)(((text[0] & 0x0Fu) << 12)
                                          | ((text[1] & 0x3Fu) << 6)
                                          | (text[2] & 0x3Fu));
        *cursor += 3;
        return point;
    }
    *cursor += 1;
    return '?';
}

static uint8_t to_cp866(uint32_t point) {
    if (point < 0x80u) {
        return (uint8_t)point;
    }
    if (point >= 0x0410u && point <= 0x042Fu) {
        return (uint8_t)(0x80u + (point - 0x0410u));   /* А..Я */
    }
    if (point >= 0x0430u && point <= 0x043Fu) {
        return (uint8_t)(0xA0u + (point - 0x0430u));   /* а..п */
    }
    if (point >= 0x0440u && point <= 0x044Fu) {
        return (uint8_t)(0xE0u + (point - 0x0440u));   /* р..я */
    }
    if (point == 0x0401u) {
        return 0xF0u;                                   /* Ё */
    }
    if (point == 0x0451u) {
        return 0xF1u;                                   /* ё */
    }
    return (uint8_t)'?';
}

/* ---------------- VGA-текст 80x25 ---------------- */

static void vga_cell(PcMachine* machine, int row, int column, uint8_t character, uint8_t attribute) {
    if (row < 0 || row >= OPENXISO_SCREEN_ROWS || column < 0 || column >= OPENXISO_SCREEN_COLUMNS) {
        return;
    }
    const size_t index = OPENXISO_VGA_ADDRESS
                         + ((size_t)row * OPENXISO_SCREEN_COLUMNS + column) * 2u;
    machine->memory[index] = character;
    machine->memory[index + 1u] = attribute;
}

static void vga_cursor(PcMachine* machine) {
    machine->memory[0x0450u] = machine->cursor_col;
    machine->memory[0x0451u] = machine->cursor_row;
}

static void vga_scroll(PcMachine* machine, uint8_t attribute) {
    const size_t line_size = OPENXISO_SCREEN_COLUMNS * 2u;
    uint8_t* base = machine->memory + OPENXISO_VGA_ADDRESS;
    memmove(base, base + line_size, line_size * (OPENXISO_SCREEN_ROWS - 1));
    for (int column = 0; column < OPENXISO_SCREEN_COLUMNS; ++column) {
        vga_cell(machine, OPENXISO_SCREEN_ROWS - 1, column, ' ', attribute);
    }
}

static void vga_newline(PcMachine* machine, uint8_t attribute) {
    machine->cursor_col = 0;
    if (machine->cursor_row + 1 >= OPENXISO_SCREEN_ROWS) {
        vga_scroll(machine, attribute);
    } else {
        machine->cursor_row += 1;
    }
    vga_cursor(machine);
}

static void vga_put(PcMachine* machine, uint8_t character, uint8_t attribute) {
    switch (character) {
        case '\n': vga_newline(machine, attribute); return;
        case '\r': machine->cursor_col = 0; vga_cursor(machine); return;
        case '\b':
            if (machine->cursor_col > 0) {
                machine->cursor_col -= 1;
                vga_cursor(machine);
            }
            return;
        default: break;
    }
    vga_cell(machine, machine->cursor_row, machine->cursor_col, character, attribute);
    machine->cursor_col += 1;
    if (machine->cursor_col >= OPENXISO_SCREEN_COLUMNS) {
        vga_newline(machine, attribute);
    } else {
        vga_cursor(machine);
    }
}

void legacy_bios_print(PcMachine* machine, const char* utf8, uint8_t attribute) {
    const char* cursor = utf8;
    for (;;) {
        const uint32_t point = next_codepoint(&cursor);
        if (point == 0) {
            break;
        }
        vga_put(machine, to_cp866(point), attribute);
    }
}

void legacy_bios_log(PcMachine* machine, const char* format, ...) {
    char line[192];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);

    /*
     * Загрузчик может сделать сотни одинаковых чтений INT 13h. Сворачиваем
     * повторы в одну строку со счётчиком, иначе журнал на экране становится
     * бесполезным и вытесняет сообщения POST.
     */
    if (machine->log_count > 0 && machine->log_repeat < 9999 &&
        strcmp(machine->log_last, line) == 0) {
        machine->log_repeat += 1;
        char suffix[24];
        snprintf(suffix, sizeof(suffix), " ×%d", machine->log_repeat + 1);
        const size_t capacity = sizeof(machine->log_lines[0]);
        const size_t suffix_length = strlen(suffix);
        size_t base = strlen(line);
        if (base + suffix_length >= capacity) {
            base = capacity - suffix_length - 1u;
        }
        char* target = machine->log_lines[machine->log_count - 1];
        memcpy(target, line, base);
        memcpy(target + base, suffix, suffix_length + 1u);
        return;
    }
    snprintf(machine->log_last, sizeof(machine->log_last), "%s", line);
    machine->log_repeat = 0;

    if (machine->log_count == OPENXISO_LOG_CAPACITY) {
        memmove(machine->log_lines[0], machine->log_lines[1],
                sizeof(machine->log_lines[0]) * (OPENXISO_LOG_CAPACITY - 1));
        machine->log_count -= 1;
    }
    snprintf(machine->log_lines[machine->log_count],
             sizeof(machine->log_lines[0]), "%s", line);
    machine->log_count += 1;
}

/* ---------------- Установка ROM, IVT и POST ---------------- */

void legacy_bios_install(PcMachine* machine) {
    /* IVT: все 256 векторов указывают на заглушки в сегменте F000. */
    for (int vector = 0; vector < 256; ++vector) {
        const uint16_t stub = (uint16_t)(0x1000u + (uint16_t)vector * 16u);
        x86_write_word(&machine->cpu, (uint32_t)vector * 4u, stub);
        x86_write_word(&machine->cpu, (uint32_t)vector * 4u + 2u, 0xF000u);

        uint8_t* code = machine->memory + OPENXISO_BIOS_ROM_ADDRESS + stub;
        memset(code, 0x90, 16);                       /* NOP */
        code[0] = 0xB0; code[1] = (uint8_t)vector;    /* mov al, vector */
        code[2] = 0xBA; code[3] = 0x10; code[4] = 0x05; /* mov dx, 0510h */
        code[5] = 0xEE;                                /* out dx, al     */
        code[6] = 0xCF;                                /* iret           */
    }

    /* Код POST по адресу F000:FE00. */
    uint8_t* post = machine->memory + OPENXISO_BIOS_ROM_ADDRESS + 0xFE00u;
    memset(post, 0x90, 32);
    post[0] = 0xB0; post[1] = 0x01;                   /* mov al, 1       */
    post[2] = 0xBA; post[3] = 0x11; post[4] = 0x05;   /* mov dx, 0511h   */
    post[5] = 0xEE;                                    /* out dx, al      */
    post[6] = 0xCD; post[7] = 0x19;                   /* int 19h         */
    post[8] = 0xF4;                                    /* hlt             */

    /* Вектор сброса F000:FFF0 -> far jmp F000:FE00. */
    uint8_t* reset = machine->memory + OPENXISO_BIOS_ROM_ADDRESS + 0xFFF0u;
    reset[0] = 0xEA; reset[1] = 0x00; reset[2] = 0xFE;
    reset[3] = 0x00; reset[4] = 0xF0;
    /* Дата сборки прошивки: ровно 8 байт до конца 1 МБ, без выхода за границу. */
    memcpy(machine->memory + OPENXISO_BIOS_ROM_ADDRESS + 0xFFF5u,
           "11/04/26", 8);
    memcpy(machine->memory + OPENXISO_BIOS_ROM_ADDRESS + 0xFE80u,
           "OpenXiso Little Legacy BIOS 0.1", 31);

    /* Подпись прошивки в начале ROM. */
    memcpy(machine->memory + OPENXISO_BIOS_ROM_ADDRESS,
           "OPENXISO LIL LEGACY BIOS", 24);
}

void legacy_bios_post(PcMachine* machine) {
    /* Область данных BIOS (BDA) по адресу 0040:0000. */
    put16(machine, 0x0040u, 0x0000u, 0x03F8u);       /* COM1 */
    put16(machine, 0x0040u, 0x0010u, 0x002Du);       /* оборудование */
    put16(machine, 0x0040u, 0x0013u, 640u);           /* базовая память, КБ */
    machine->memory[0x0449u] = 0x03;                  /* видеорежим 80x25 */
    put16(machine, 0x0040u, 0x004Au, 80u);            /* столбцы */
    put16(machine, 0x0040u, 0x0060u, 0x0607u);        /* форма курсора */
    machine->memory[0x0484u] = 24;                    /* строк минус один */

    for (int row = 0; row < OPENXISO_SCREEN_ROWS; ++row) {
        for (int column = 0; column < OPENXISO_SCREEN_COLUMNS; ++column) {
            vga_cell(machine, row, column, ' ', 0x07u);
        }
    }
    machine->cursor_row = 0;
    machine->cursor_col = 0;
    vga_cursor(machine);

    legacy_bios_print(machine, "OpenXiso Little Legacy BIOS 0.1\r\n", 0x1Fu);
    legacy_bios_print(machine, "Память 640 КБ, VGA 80x25, ЦПУ 8086\r\n", 0x07u);
    legacy_bios_print(machine, "POST завершён, поиск загрузочного образа...\r\n", 0x07u);

    legacy_bios_log(machine, "POST: собственный Legacy BIOS 0.1 инициализирован");
    legacy_bios_log(machine, "POST: базовая память 640 КБ, видеорежим 80x25");
    machine->status = OPENXISO_MACHINE_RUNNING;
}

/* ---------------- INT 10h: видео ---------------- */

static void bios_video(PcMachine* machine) {
    const uint8_t function = reg8(machine, 4);       /* AH */
    const uint8_t character = reg8(machine, 0);      /* AL */
    const uint8_t attribute = reg8(machine, 3);      /* BL */

    switch (function) {
        case 0x00:
            machine->memory[0x0449u] = character;
            if ((character & 0x7Fu) <= 0x03u) {
                for (int row = 0; row < OPENXISO_SCREEN_ROWS; ++row) {
                    for (int column = 0; column < OPENXISO_SCREEN_COLUMNS; ++column) {
                        vga_cell(machine, row, column, ' ', 0x07u);
                    }
                }
                machine->cursor_row = 0;
                machine->cursor_col = 0;
                vga_cursor(machine);
            }
            break;
        case 0x02:
            machine->cursor_row = reg8(machine, 6);  /* DH */
            machine->cursor_col = reg8(machine, 2);  /* DL */
            vga_cursor(machine);
            break;
        case 0x03:
            set_reg8(machine, 6, machine->cursor_row);
            set_reg8(machine, 2, machine->cursor_col);
            machine->cpu.regs[X86_CX] = 0x0607u;
            break;
        case 0x06: case 0x07: {
            uint8_t lines = character == 0 ? OPENXISO_SCREEN_ROWS : character;
            for (uint8_t line = 0; line < lines; ++line) {
                vga_scroll(machine, 0x07u);
            }
            break;
        }
        case 0x09: {
            for (uint16_t index = 0; index < machine->cpu.regs[X86_CX]; ++index) {
                vga_cell(machine, machine->cursor_row,
                         (machine->cursor_col + index) % OPENXISO_SCREEN_COLUMNS,
                         character, attribute);
            }
            break;
        }
        case 0x0A:
            for (uint16_t index = 0; index < machine->cpu.regs[X86_CX]; ++index) {
                vga_cell(machine, machine->cursor_row,
                         (machine->cursor_col + index) % OPENXISO_SCREEN_COLUMNS,
                         character, 0x07u);
            }
            break;
        case 0x0E:
            vga_put(machine, character, 0x07u);
            break;
        case 0x0F:
            set_reg8(machine, 0, 0x03u);
            set_reg8(machine, 4, 80u);
            set_reg8(machine, 7, 0u);
            break;
        default:
            break;
    }
    carry(machine, 0);
}

/* ---------------- INT 13h: диск ---------------- */

static void transfer_to_memory(PcMachine* machine, uint16_t segment, uint16_t offset,
                               const uint8_t* source, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        put8(machine, segment, (uint16_t)(offset + index), source[index]);
    }
}

static void bios_read_chs(PcMachine* machine) {
    const uint8_t count = reg8(machine, 0);
    const uint8_t cylinder_low = reg8(machine, 5);   /* CH */
    const uint8_t cylinder_sector = reg8(machine, 1);/* CL */
    const uint8_t head = reg8(machine, 6);           /* DH */
    const uint16_t cylinder = (uint16_t)(cylinder_low | ((cylinder_sector & 0xC0u) << 2));
    const uint16_t sector = (uint16_t)(cylinder_sector & 0x3Fu);

    /*
     * Геометрия берётся из плана (BPB эмулируемого диска), а не из жёстко
     * прошитых 63/16: у дискеты 18 секторов на дорожку и 2 головки.
     */
    const uint16_t per_track = machine->plan.sectors_per_track != 0
                               ? machine->plan.sectors_per_track : 63u;
    const uint16_t heads = machine->plan.heads != 0 ? machine->plan.heads : 16u;
    const uint64_t lba = ((uint64_t)cylinder * heads + head) * per_track
                         + (sector > 0 ? sector - 1u : 0u);

    const int length = openxiso_disk_read(machine->fd, &machine->plan, lba, count,
                                          machine->transfer_buffer,
                                          sizeof(machine->transfer_buffer));
    if (length < 0) {
        set_reg8(machine, 4, 0x04u);                /* сектор не найден */
        carry(machine, 1);
        legacy_bios_log(machine, "INT 13h CHS: %s", openxiso_boot_last_error());
        return;
    }
    transfer_to_memory(machine, machine->cpu.sregs[X86_ES],
                       machine->cpu.regs[X86_BX], machine->transfer_buffer,
                       (size_t)length);
    set_reg8(machine, 0, count);
    set_reg8(machine, 4, 0x00u);
    carry(machine, 0);
}

static void bios_read_extended(PcMachine* machine) {
    const uint16_t packet_segment = machine->cpu.sregs[X86_DS];
    const uint16_t packet_offset = machine->cpu.regs[X86_SI];
    const uint16_t count = mem16(machine, packet_segment, (uint16_t)(packet_offset + 2u));
    const uint16_t buffer_offset = mem16(machine, packet_segment, (uint16_t)(packet_offset + 4u));
    const uint16_t buffer_segment = mem16(machine, packet_segment, (uint16_t)(packet_offset + 6u));
    const uint32_t lba_low = (uint32_t)mem16(machine, packet_segment,
                                             (uint16_t)(packet_offset + 8u))
                             | ((uint32_t)mem16(machine, packet_segment,
                                                (uint16_t)(packet_offset + 10u)) << 16);
    const uint32_t lba_high = (uint32_t)mem16(machine, packet_segment,
                                              (uint16_t)(packet_offset + 12u))
                              | ((uint32_t)mem16(machine, packet_segment,
                                                 (uint16_t)(packet_offset + 14u)) << 16);
    const uint64_t lba = (uint64_t)lba_low | ((uint64_t)lba_high << 32);

    const uint32_t sector_size = openxiso_disk_sector_size(&machine->plan);
    const int length = openxiso_disk_read(machine->fd, &machine->plan, lba, count,
                                          machine->transfer_buffer,
                                          sizeof(machine->transfer_buffer));
    if (length < 0) {
        /* EDD: обнуляем счётчик блоков в пакете и поднимаем CF. */
        x86_write_word(&machine->cpu,
                       x86_physical(packet_segment, (uint16_t)(packet_offset + 2u)),
                       0u);
        set_reg8(machine, 4, 0x20u);
        carry(machine, 1);
        legacy_bios_log(machine, "INT 13h EDD: %s", openxiso_boot_last_error());
        return;
    }
    transfer_to_memory(machine, buffer_segment, buffer_offset,
                       machine->transfer_buffer, (size_t)length);
    /* По спецификации EDD факт переданных блоков возвращается в пакете. */
    const uint16_t transferred = (uint16_t)(length / (int)sector_size);
    x86_write_word(&machine->cpu,
                   x86_physical(packet_segment, (uint16_t)(packet_offset + 2u)),
                   transferred);
    set_reg8(machine, 0, (uint8_t)transferred);
    set_reg8(machine, 4, 0x00u);
    carry(machine, 0);
}

static void bios_disk(PcMachine* machine) {
    const uint8_t function = reg8(machine, 4);
    switch (function) {
        case 0x00:                                     /* сброс контроллера */
            set_reg8(machine, 4, 0x00u);
            carry(machine, 0);
            break;
        case 0x02:
            bios_read_chs(machine);
            break;
        case 0x08: {                                   /* геометрия привода */
            const uint16_t per_track = machine->plan.sectors_per_track != 0
                                       ? machine->plan.sectors_per_track : 63u;
            const uint16_t heads = machine->plan.heads != 0 ? machine->plan.heads : 16u;
            uint32_t cylinders = (uint32_t)(openxiso_disk_total_sectors(&machine->plan)
                                            / ((uint64_t)per_track * heads));
            if (cylinders > 1023u) {
                cylinders = 1023u;
            }
            set_reg8(machine, 1, (uint8_t)((per_track & 0x3Fu)
                                           | ((cylinders >> 2) & 0xC0u)));
            set_reg8(machine, 5, (uint8_t)(cylinders & 0xFFu));
            set_reg8(machine, 6, (uint8_t)(heads - 1u));
            set_reg8(machine, 2, 1u);                  /* один привод */
            set_reg8(machine, 4, 0x00u);
            machine->cpu.sregs[X86_ES] = 0xF000u;
            machine->cpu.regs[X86_DI] = 0x0000u;
            carry(machine, 0);
            break;
        }
        case 0x41:                                     /* проверка EDD */
            machine->cpu.regs[X86_BX] = 0xAA55u;
            set_reg8(machine, 4, 0x21u);
            machine->cpu.regs[X86_CX] = 0x0001u;
            carry(machine, 0);
            break;
        case 0x42:
            bios_read_extended(machine);
            legacy_bios_log(machine, "INT 13h: чтение LBA (%s)",
                            openxiso_emulation_name(machine->plan.emulation));
            break;
        case 0x48: {                                   /* параметры EDD */
            const uint16_t segment = machine->cpu.sregs[X86_DS];
            const uint16_t offset = machine->cpu.regs[X86_SI];
            memset(machine->transfer_buffer, 0, 0x1Eu);
            machine->transfer_buffer[0] = 0x1Eu;
            const uint32_t sector_size = openxiso_disk_sector_size(&machine->plan);
            const uint64_t total = openxiso_disk_total_sectors(&machine->plan);
            for (int index = 0; index < 8; ++index) {
                machine->transfer_buffer[0x10u + index] = (uint8_t)(total >> (8 * index));
            }
            machine->transfer_buffer[0x18u] = (uint8_t)(sector_size & 0xFFu);
            machine->transfer_buffer[0x19u] = (uint8_t)(sector_size >> 8);
            transfer_to_memory(machine, segment, offset, machine->transfer_buffer, 0x1Eu);
            set_reg8(machine, 4, 0x00u);
            carry(machine, 0);
            break;
        }
        default:
            set_reg8(machine, 4, 0x01u);
            carry(machine, 1);
            break;
    }
}

/* ---------------- INT 16h: клавиатура ---------------- */

static void bios_keyboard(PcMachine* machine) {
    const uint8_t function = reg8(machine, 4);
    if (function == 0x00 || function == 0x10) {
        if (machine->key_head == machine->key_tail) {
            machine->cpu.waiting = 1;                 /* IP отматывается в x86_step */
            machine->status = OPENXISO_MACHINE_WAITING;
            return;
        }
        const uint16_t key = machine->keys[machine->key_head];
        machine->key_head = (machine->key_head + 1) % 64;
        set_reg8(machine, 0, (uint8_t)(key & 0xFFu));
        set_reg8(machine, 4, (uint8_t)(key >> 8));
        x86_set_zero(&machine->cpu, 0);
        return;
    }
    if (function == 0x01 || function == 0x11) {
        if (machine->key_head == machine->key_tail) {
            x86_set_zero(&machine->cpu, 1);
        } else {
            const uint16_t key = machine->keys[machine->key_head];
            set_reg8(machine, 0, (uint8_t)(key & 0xFFu));
            set_reg8(machine, 4, (uint8_t)(key >> 8));
            x86_set_zero(&machine->cpu, 0);
        }
        return;
    }
    if (function == 0x02) {
        set_reg8(machine, 0, 0x00u);
    }
}

/* ---------------- INT 19h: передача управления загрузчику ---------------- */

static void bios_bootstrap(PcMachine* machine) {
    uint8_t* boot_area = machine->memory + OPENXISO_BOOT_ADDRESS;
    const int loaded = openxiso_boot_load(machine->fd, &machine->plan,
                                          boot_area, 4096u);
    if (loaded < 0) {
        snprintf(machine->error, sizeof(machine->error), "%s",
                 openxiso_boot_last_error());
        legacy_bios_log(machine, "Ошибка загрузки: %s", machine->error);
        machine->status = OPENXISO_MACHINE_ERROR;
        machine->cpu.halted = 1;
        return;
    }

    /* Регистры, которые ожидает загрузчик: DL = привод, DS=ES=SS=0. */
    machine->cpu.regs[X86_DX] = (uint16_t)((machine->cpu.regs[X86_DX] & 0xFF00u)
                                           | machine->plan.drive);
    machine->cpu.sregs[X86_ES] = 0x0000u;
    machine->cpu.sregs[X86_DS] = 0x0000u;
    machine->cpu.regs[X86_SI] = OPENXISO_BOOT_ADDRESS;
    machine->cpu.regs[X86_BP] = 0x0000u;

    /* Повторный INT 19h — это перезагрузка: сбрасываем и широкое ядро. */
    machine->cpu.mode = X86_MODE_16;
    machine->cpu.cr0 = 0u;
    machine->cpu.efer = 0u;
    machine->cpu.rip = OPENXISO_BOOT_ADDRESS;
    machine->cpu.seg_base[X86_CS] = 0u;
    machine->cpu.seg_base[X86_DS] = 0u;
    machine->cpu.seg_base[X86_ES] = 0u;
    machine->cpu.seg_base[X86_SS] = 0u;

    /*
     * Firmware-хук INT 19h обработал вызов без кадра iret, поэтому управление
     * передаём напрямую: настоящий BIOS после загрузки MBR делает то же самое —
     * far jmp 0000:7C00.
     */
    machine->cpu.sregs[X86_CS] = 0x0000u;
    machine->cpu.ip = OPENXISO_BOOT_ADDRESS;
    carry(machine, 0);

    legacy_bios_log(machine,
                    machine->plan.el_torito
                        ? "El Torito: загрузчик прочитан, управление передано 0000:7C00"
                        : "MBR: загрузчик прочитан, управление передано 0000:7C00");
    legacy_bios_log(machine, "Диск: %s, сектор %u байт, DL=%02Xh",
                    machine->plan.el_torito
                        ? openxiso_emulation_name(machine->plan.emulation)
                        : "сырой образ MBR",
                    openxiso_disk_sector_size(&machine->plan), machine->plan.drive);
    legacy_bios_print(machine, "Загрузка образа...\r\n", 0x0Au);
    machine->status = OPENXISO_MACHINE_RUNNING;
}

/* ---------------- Прочие сервисы ---------------- */

static void bios_system(PcMachine* machine) {
    const uint8_t function = reg8(machine, 4);
    if (function == 0x88) {
        machine->cpu.regs[X86_AX] = 0x0000u;
        carry(machine, 0);
    } else {
        set_reg8(machine, 4, 0x86u);
        carry(machine, 1);
    }
}

static void bios_time(PcMachine* machine) {
    if (reg8(machine, 4) == 0x00u) {
        machine->cpu.regs[X86_CX] = (uint16_t)(machine->ticks >> 16);
        machine->cpu.regs[X86_DX] = (uint16_t)(machine->ticks & 0xFFFFu);
        set_reg8(machine, 0, 0x00u);
    }
}

void legacy_bios_interrupt(PcMachine* machine, uint8_t vector) {
    switch (vector) {
        case 0x08:
            machine->ticks += 1u;
            x86_write_word(&machine->cpu, 0x046Cu,
                           (uint16_t)(machine->ticks & 0xFFFFu));
            break;
        case 0x10: bios_video(machine); break;
        case 0x11:
            machine->cpu.regs[X86_AX] = mem16(machine, 0x0040u, 0x0010u);
            break;
        case 0x12:
            machine->cpu.regs[X86_AX] = 640u;
            break;
        case 0x13: bios_disk(machine); break;
        case 0x15: bios_system(machine); break;
        case 0x16: bios_keyboard(machine); break;
        case 0x19: bios_bootstrap(machine); break;
        case 0x1A: bios_time(machine); break;
        case 0x1C: break;
        default:
            legacy_bios_log(machine, "BIOS: неизвестный вызов INT %02Xh", vector);
            break;
    }
}
