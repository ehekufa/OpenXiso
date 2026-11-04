/*
 * x86_cpu.c — интерпретатор реального режима 8086/186 (собственная реализация).
 *
 * Поддерживаются: все основные арифметико-логические операции, ModRM-адресация,
 * префиксы сегментов/REP/LOCK, стек, переходы, вызовы, строковые операции,
 * сдвиги, порты ввода-вывода и прерывания. Память — плоский 1 МБ с отключённой
 * линией A20, как у настоящего PC в реальном режиме.
 * Комментарии и диагностика — на русском языке.
 */

#include "include/x86_cpu.h"

#include <string.h>

#define ADDRESS_MASK 0xFFFFFu

/* ---------------- Флаги ---------------- */

static void set_flag(X86Cpu* cpu, uint16_t mask, int value) {
    if (value) {
        cpu->flags |= mask;
    } else {
        cpu->flags &= (uint16_t)~mask;
    }
}

void x86_set_carry(X86Cpu* cpu, int value) { set_flag(cpu, X86_CF, value); }
void x86_set_zero(X86Cpu* cpu, int value) { set_flag(cpu, X86_ZF, value); }

static int flag(const X86Cpu* cpu, uint16_t mask) {
    return (cpu->flags & mask) != 0;
}

static int parity(uint8_t value) {
    int bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits += (value >> i) & 1;
    }
    return (bits & 1) == 0;
}

/* ---------------- Память ---------------- */

uint32_t x86_physical(uint16_t segment, uint16_t offset) {
    return (((uint32_t)segment << 4) + offset) & ADDRESS_MASK;
}

uint8_t x86_read_byte(const X86Cpu* cpu, uint32_t physical) {
    return cpu->memory[physical & ADDRESS_MASK];
}

void x86_write_byte(X86Cpu* cpu, uint32_t physical, uint8_t value) {
    cpu->memory[physical & ADDRESS_MASK] = value;
}

uint16_t x86_read_word(const X86Cpu* cpu, uint32_t physical) {
    return (uint16_t)(x86_read_byte(cpu, physical)
                      | ((uint16_t)x86_read_byte(cpu, physical + 1u) << 8));
}

void x86_write_word(X86Cpu* cpu, uint32_t physical, uint16_t value) {
    x86_write_byte(cpu, physical, (uint8_t)(value & 0xFFu));
    x86_write_byte(cpu, physical + 1u, (uint8_t)(value >> 8));
}

static uint8_t mem_read8(const X86Cpu* cpu, uint16_t segment, uint16_t offset) {
    return x86_read_byte(cpu, x86_physical(segment, offset));
}

static void mem_write8(X86Cpu* cpu, uint16_t segment, uint16_t offset, uint8_t value) {
    x86_write_byte(cpu, x86_physical(segment, offset), value);
}

static uint16_t mem_read16(const X86Cpu* cpu, uint16_t segment, uint16_t offset) {
    return x86_read_word(cpu, x86_physical(segment, offset));
}

static void mem_write16(X86Cpu* cpu, uint16_t segment, uint16_t offset, uint16_t value) {
    x86_write_word(cpu, x86_physical(segment, offset), value);
}

/* ---------------- Выборка инструкций ---------------- */

static uint8_t fetch8(X86Cpu* cpu) {
    const uint8_t value = mem_read8(cpu, cpu->sregs[X86_CS], cpu->ip);
    cpu->ip = (uint16_t)(cpu->ip + 1u);
    return value;
}

static uint16_t fetch16(X86Cpu* cpu) {
    const uint8_t low = fetch8(cpu);
    const uint8_t high = fetch8(cpu);
    return (uint16_t)(low | ((uint16_t)high << 8));
}

static int8_t fetch_s8(X86Cpu* cpu) { return (int8_t)fetch8(cpu); }
static int16_t fetch_s16(X86Cpu* cpu) { return (int16_t)fetch16(cpu); }

/* ---------------- Регистры ---------------- */

static uint8_t get_reg8(const X86Cpu* cpu, int index) {
    const uint16_t value = cpu->regs[index & 3];
    return index < 4 ? (uint8_t)(value & 0xFFu) : (uint8_t)(value >> 8);
}

static void set_reg8(X86Cpu* cpu, int index, uint8_t value) {
    uint16_t* target = &cpu->regs[index & 3];
    if (index < 4) {
        *target = (uint16_t)((*target & 0xFF00u) | value);
    } else {
        *target = (uint16_t)((*target & 0x00FFu) | ((uint16_t)value << 8));
    }
}

/* ---------------- ModRM ---------------- */

typedef struct ModRM {
    int mod;
    int reg;
    int rm;
    int is_register;
    uint16_t offset;
    uint16_t segment;
} ModRM;

static ModRM decode_modrm(X86Cpu* cpu, int segment_override) {
    const uint8_t byte = fetch8(cpu);
    ModRM result;
    memset(&result, 0, sizeof(result));
    result.mod = byte >> 6;
    result.reg = (byte >> 3) & 7;
    result.rm = byte & 7;
    result.is_register = result.mod == 3;

    if (!result.is_register) {
        uint16_t displacement = 0;
        int uses_bp = 0;
        switch (result.rm) {
            case 0: displacement = (uint16_t)(cpu->regs[X86_BX] + cpu->regs[X86_SI]); break;
            case 1: displacement = (uint16_t)(cpu->regs[X86_BX] + cpu->regs[X86_DI]); break;
            case 2: displacement = (uint16_t)(cpu->regs[X86_BP] + cpu->regs[X86_SI]); uses_bp = 1; break;
            case 3: displacement = (uint16_t)(cpu->regs[X86_BP] + cpu->regs[X86_DI]); uses_bp = 1; break;
            case 4: displacement = cpu->regs[X86_SI]; break;
            case 5: displacement = cpu->regs[X86_DI]; break;
            case 6:
                if (result.mod == 0) {
                    displacement = fetch16(cpu);
                } else {
                    displacement = cpu->regs[X86_BP];
                    uses_bp = 1;
                }
                break;
            default: displacement = cpu->regs[X86_BX]; break;
        }
        if (result.mod == 1) {
            displacement = (uint16_t)(displacement + (int16_t)fetch_s8(cpu));
        } else if (result.mod == 2) {
            displacement = (uint16_t)(displacement + fetch16(cpu));
        }
        result.offset = displacement;
        const int segment_index = segment_override >= 0
                                  ? segment_override
                                  : (uses_bp ? X86_SS : X86_DS);
        result.segment = cpu->sregs[segment_index];
    }
    return result;
}

static uint16_t read_rm(X86Cpu* cpu, const ModRM* modrm, int width) {
    if (modrm->is_register) {
        return width == 8 ? get_reg8(cpu, modrm->rm) : cpu->regs[modrm->rm];
    }
    const uint32_t address = x86_physical(modrm->segment, modrm->offset);
    return width == 8
           ? x86_read_byte(cpu, address)
           : x86_read_word(cpu, address);
}

static void write_rm(X86Cpu* cpu, const ModRM* modrm, int width, uint16_t value) {
    if (modrm->is_register) {
        if (width == 8) {
            set_reg8(cpu, modrm->rm, (uint8_t)value);
        } else {
            cpu->regs[modrm->rm] = value;
        }
        return;
    }
    const uint32_t address = x86_physical(modrm->segment, modrm->offset);
    if (width == 8) {
        x86_write_byte(cpu, address, (uint8_t)value);
    } else {
        x86_write_word(cpu, address, value);
    }
}

/* ---------------- ALU ---------------- */

static uint16_t alu(X86Cpu* cpu, int operation, uint16_t a, uint16_t b, int width) {
    const uint16_t mask = width == 8 ? 0x00FFu : 0xFFFFu;
    const uint16_t sign = width == 8 ? 0x0080u : 0x8000u;
    const uint16_t carry_in = (operation == 2 || operation == 3) && flag(cpu, X86_CF) ? 1u : 0u;
    uint32_t wide = 0;

    switch (operation) {
        case 0: wide = (uint32_t)a + b; break;                    /* ADD */
        case 1: wide = (uint32_t)(a | b); break;                  /* OR  */
        case 2: wide = (uint32_t)a + b + carry_in; break;         /* ADC */
        case 3: wide = (uint32_t)a - b - carry_in; break;         /* SBB */
        case 4: wide = (uint32_t)(a & b); break;                  /* AND */
        case 5: wide = (uint32_t)a - b; break;                    /* SUB */
        case 6: wide = (uint32_t)(a ^ b); break;                  /* XOR */
        default: wide = (uint32_t)a - b; break;                   /* CMP */
    }

    const uint16_t result = (uint16_t)(wide & mask);
    if (operation == 0 || operation == 2) {
        set_flag(cpu, X86_CF, wide > mask);
    } else if (operation == 3 || operation == 5 || operation == 7) {
        set_flag(cpu, X86_CF, (uint32_t)a < (uint32_t)b + carry_in);
    } else {
        set_flag(cpu, X86_CF, 0);
    }

    int overflow = 0;
    if (operation == 0 || operation == 2) {
        overflow = ((~(a ^ b) & (a ^ result)) & sign) != 0;
    } else if (operation == 3 || operation == 5 || operation == 7) {
        overflow = (((a ^ b) & (a ^ result)) & sign) != 0;
    }
    set_flag(cpu, X86_OF, overflow);
    set_flag(cpu, X86_AF, ((a ^ b ^ result) & 0x10u) != 0);
    set_flag(cpu, X86_ZF, result == 0);
    set_flag(cpu, X86_SF, (result & sign) != 0);
    set_flag(cpu, X86_PF, parity((uint8_t)result));
    return result;
}

static uint16_t inc_dec(X86Cpu* cpu, uint16_t value, int width, int increment) {
    const int carry = flag(cpu, X86_CF);
    const uint16_t result = alu(cpu, increment ? 0 : 5, value, 1, width);
    set_flag(cpu, X86_CF, carry);
    return result;
}

/* ---------------- Сдвиги ---------------- */

static uint16_t shift(X86Cpu* cpu, int operation, uint16_t value, uint8_t count, int width) {
    const uint16_t mask = width == 8 ? 0x00FFu : 0xFFFFu;
    const uint16_t sign = width == 8 ? 0x0080u : 0x8000u;
    count &= 0x1Fu;
    value &= mask;
    if (count == 0) {
        return value;
    }

    uint32_t work = value;
    int carry = 0;
    switch (operation) {
        case 0: /* ROL */
            for (uint8_t i = 0; i < count; ++i) {
                carry = (work & sign) != 0;
                work = ((work << 1) | (uint32_t)carry) & mask;
            }
            break;
        case 1: /* ROR */
            for (uint8_t i = 0; i < count; ++i) {
                carry = work & 1;
                work = ((work >> 1) | ((uint32_t)carry * sign)) & mask;
            }
            break;
        case 2: /* RCL */
            for (uint8_t i = 0; i < count; ++i) {
                const int old_carry = flag(cpu, X86_CF);
                carry = (work & sign) != 0;
                work = ((work << 1) | (uint32_t)old_carry) & mask;
                set_flag(cpu, X86_CF, carry);
            }
            break;
        case 3: /* RCR */
            for (uint8_t i = 0; i < count; ++i) {
                const int old_carry = flag(cpu, X86_CF);
                carry = work & 1;
                work = (work >> 1) | ((uint32_t)old_carry * sign);
                set_flag(cpu, X86_CF, carry);
            }
            break;
        case 4: /* SHL/SAL */
        case 6:
            for (uint8_t i = 0; i < count; ++i) {
                carry = (work & sign) != 0;
                work = (work << 1) & mask;
            }
            break;
        case 5: /* SHR */
            for (uint8_t i = 0; i < count; ++i) {
                carry = work & 1;
                work >>= 1;
            }
            break;
        default: /* SAR */
            for (uint8_t i = 0; i < count; ++i) {
                carry = work & 1;
                work = (work >> 1) | (work & sign);
            }
            break;
    }
    set_flag(cpu, X86_CF, carry);
    const uint16_t result = (uint16_t)(work & mask);
    if (count == 1 && operation >= 4) {
        set_flag(cpu, X86_OF, ((value ^ result) & sign) != 0 && operation != 5);
        if (operation == 5) {
            set_flag(cpu, X86_OF, (value & sign) != 0);
        }
    }
    if (operation >= 4) {
        set_flag(cpu, X86_ZF, result == 0);
        set_flag(cpu, X86_SF, (result & sign) != 0);
        set_flag(cpu, X86_PF, parity((uint8_t)result));
    }
    return result;
}

/* ---------------- Стек и прерывания ---------------- */

void x86_push(X86Cpu* cpu, uint16_t value) {
    cpu->regs[X86_SP] = (uint16_t)(cpu->regs[X86_SP] - 2u);
    mem_write16(cpu, cpu->sregs[X86_SS], cpu->regs[X86_SP], value);
}

uint16_t x86_pop(X86Cpu* cpu) {
    const uint16_t value = mem_read16(cpu, cpu->sregs[X86_SS], cpu->regs[X86_SP]);
    cpu->regs[X86_SP] = (uint16_t)(cpu->regs[X86_SP] + 2u);
    return value;
}

void x86_raise_interrupt(X86Cpu* cpu, uint8_t vector) {
    if (cpu->interrupt_hook && cpu->interrupt_hook(cpu->context, vector)) {
        return;
    }
    x86_push(cpu, cpu->flags);
    set_flag(cpu, X86_IF, 0);
    set_flag(cpu, X86_TF, 0);
    x86_push(cpu, cpu->sregs[X86_CS]);
    x86_push(cpu, cpu->ip);
    const uint32_t entry = (uint32_t)vector * 4u;
    cpu->ip = x86_read_word(cpu, entry);
    cpu->sregs[X86_CS] = x86_read_word(cpu, entry + 2u);
}

void x86_reset(X86Cpu* cpu, uint8_t* memory, size_t memory_size) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->memory = memory;
    cpu->memory_size = memory_size;
    cpu->flags = 0x0002u;
}

/* ---------------- Порты ---------------- */

static uint8_t port_in(X86Cpu* cpu, uint16_t port) {
    return cpu->port_in ? cpu->port_in(cpu->context, port) : 0xFFu;
}

static void port_out(X86Cpu* cpu, uint16_t port, uint8_t value) {
    if (cpu->port_out) {
        cpu->port_out(cpu->context, port, value);
    }
}

/* ---------------- Условия переходов ---------------- */

static int condition(const X86Cpu* cpu, int code) {
    switch (code & 0x0F) {
        case 0: return flag(cpu, X86_OF);                          /* JO   */
        case 1: return !flag(cpu, X86_OF);                         /* JNO  */
        case 2: return flag(cpu, X86_CF);                          /* JB   */
        case 3: return !flag(cpu, X86_CF);                         /* JAE  */
        case 4: return flag(cpu, X86_ZF);                          /* JZ   */
        case 5: return !flag(cpu, X86_ZF);                         /* JNZ  */
        case 6: return flag(cpu, X86_CF) || flag(cpu, X86_ZF);     /* JBE  */
        case 7: return !flag(cpu, X86_CF) && !flag(cpu, X86_ZF);   /* JA   */
        case 8: return flag(cpu, X86_SF);                          /* JS   */
        case 9: return !flag(cpu, X86_SF);                         /* JNS  */
        case 10: return flag(cpu, X86_PF);                         /* JP   */
        case 11: return !flag(cpu, X86_PF);                        /* JNP  */
        case 12: return flag(cpu, X86_SF) != flag(cpu, X86_OF);    /* JL   */
        case 13: return flag(cpu, X86_SF) == flag(cpu, X86_OF);    /* JGE  */
        case 14: return flag(cpu, X86_ZF) || (flag(cpu, X86_SF) != flag(cpu, X86_OF));
        default: return !flag(cpu, X86_ZF) && (flag(cpu, X86_SF) == flag(cpu, X86_OF));
    }
}

/* ---------------- Строковые операции ---------------- */

static void string_operation(X86Cpu* cpu, uint8_t opcode, int repetition, int segment_override) {
    const int width = (opcode & 1u) ? 16 : 8;
    const int delta = (width == 8) ? 1 : 2;
    const int step = flag(cpu, X86_DF) ? -delta : delta;
    const uint16_t source_segment = segment_override >= 0
                                    ? cpu->sregs[segment_override]
                                    : cpu->sregs[X86_DS];
    const uint16_t dest_segment = cpu->sregs[X86_ES];
    uint32_t count = repetition ? cpu->regs[X86_CX] : 1u;

    while (count > 0) {
        uint16_t source = 0;
        uint16_t destination = 0;
        switch (opcode) {
            case 0xA4: case 0xA5: /* MOVS */
                source = width == 8
                         ? mem_read8(cpu, source_segment, cpu->regs[X86_SI])
                         : mem_read16(cpu, source_segment, cpu->regs[X86_SI]);
                if (width == 8) {
                    mem_write8(cpu, dest_segment, cpu->regs[X86_DI], (uint8_t)source);
                } else {
                    mem_write16(cpu, dest_segment, cpu->regs[X86_DI], source);
                }
                cpu->regs[X86_SI] = (uint16_t)(cpu->regs[X86_SI] + step);
                cpu->regs[X86_DI] = (uint16_t)(cpu->regs[X86_DI] + step);
                break;
            case 0xA6: case 0xA7: /* CMPS */
                source = width == 8
                         ? mem_read8(cpu, source_segment, cpu->regs[X86_SI])
                         : mem_read16(cpu, source_segment, cpu->regs[X86_SI]);
                destination = width == 8
                              ? mem_read8(cpu, dest_segment, cpu->regs[X86_DI])
                              : mem_read16(cpu, dest_segment, cpu->regs[X86_DI]);
                alu(cpu, 7, source, destination, width);
                cpu->regs[X86_SI] = (uint16_t)(cpu->regs[X86_SI] + step);
                cpu->regs[X86_DI] = (uint16_t)(cpu->regs[X86_DI] + step);
                break;
            case 0xAA: case 0xAB: /* STOS */
                source = width == 8 ? get_reg8(cpu, 0) : cpu->regs[X86_AX];
                if (width == 8) {
                    mem_write8(cpu, dest_segment, cpu->regs[X86_DI], (uint8_t)source);
                } else {
                    mem_write16(cpu, dest_segment, cpu->regs[X86_DI], source);
                }
                cpu->regs[X86_DI] = (uint16_t)(cpu->regs[X86_DI] + step);
                break;
            case 0xAC: case 0xAD: /* LODS */
                source = width == 8
                         ? mem_read8(cpu, source_segment, cpu->regs[X86_SI])
                         : mem_read16(cpu, source_segment, cpu->regs[X86_SI]);
                if (width == 8) {
                    set_reg8(cpu, 0, (uint8_t)source);
                } else {
                    cpu->regs[X86_AX] = source;
                }
                cpu->regs[X86_SI] = (uint16_t)(cpu->regs[X86_SI] + step);
                break;
            default: /* SCAS */
                source = width == 8 ? get_reg8(cpu, 0) : cpu->regs[X86_AX];
                destination = width == 8
                              ? mem_read8(cpu, dest_segment, cpu->regs[X86_DI])
                              : mem_read16(cpu, dest_segment, cpu->regs[X86_DI]);
                alu(cpu, 7, source, destination, width);
                cpu->regs[X86_DI] = (uint16_t)(cpu->regs[X86_DI] + step);
                break;
        }
        --count;
        if (repetition) {
            cpu->regs[X86_CX] = (uint16_t)(cpu->regs[X86_CX] - 1u);
        }
        if (repetition && (opcode == 0xA6 || opcode == 0xA7 || opcode == 0xAE || opcode == 0xAF)) {
            const int zero = flag(cpu, X86_ZF);
            if ((repetition > 0 && !zero) || (repetition < 0 && zero)) {
                break;
            }
        }
    }
}

/* ---------------- Одна инструкция ---------------- */

int x86_step(X86Cpu* cpu) {
    if (cpu->halted || cpu->waiting || cpu->fault) {
        return 0;
    }

    const uint16_t start_ip = cpu->ip;
    int repetition = 0;
    int segment_override = -1;
    uint8_t opcode = 0;

    for (;;) {
        opcode = fetch8(cpu);
        switch (opcode) {
            case 0xF0: continue;                                   /* LOCK */
            case 0xF2: repetition = -1; continue;                  /* REPNE */
            case 0xF3: repetition = 1; continue;                   /* REP/REPE */
            case 0x26: segment_override = X86_ES; continue;
            case 0x2E: segment_override = X86_CS; continue;
            case 0x36: segment_override = X86_SS; continue;
            case 0x3E: segment_override = X86_DS; continue;
            default: break;
        }
        break;
    }

    cpu->instructions += 1u;

    /* Групповая арифметика 0x00..0x3D */
    if (opcode <= 0x3Du && (opcode & 0x07u) <= 0x05u) {
        const int operation = opcode >> 3;
        const int width = (opcode & 1u) ? 16 : 8;
        if ((opcode & 0x07u) <= 0x03u) {
            const ModRM modrm = decode_modrm(cpu, segment_override);
            const int direction = opcode & 0x02u;
            uint16_t destination = read_rm(cpu, &modrm, width);
            uint16_t source = width == 8 ? get_reg8(cpu, modrm.reg) : cpu->regs[modrm.reg];
            if (direction) {
                source = destination;
                destination = width == 8 ? get_reg8(cpu, modrm.reg) : cpu->regs[modrm.reg];
                const uint16_t result = alu(cpu, operation, destination, source, width);
                if (width == 8) {
                    set_reg8(cpu, modrm.reg, (uint8_t)result);
                } else {
                    cpu->regs[modrm.reg] = result;
                }
            } else {
                const uint16_t result = alu(cpu, operation, destination, source, width);
                write_rm(cpu, &modrm, width, result);
            }
        } else {
            const uint16_t immediate = width == 8 ? fetch8(cpu) : fetch16(cpu);
            const uint16_t accumulator = width == 8 ? get_reg8(cpu, 0) : cpu->regs[X86_AX];
            const uint16_t result = alu(cpu, operation, accumulator, immediate, width);
            if (operation != 7) {
                if (width == 8) {
                    set_reg8(cpu, 0, (uint8_t)result);
                } else {
                    cpu->regs[X86_AX] = result;
                }
            }
        }
        return 1;
    }

    switch (opcode) {
        /* PUSH/POP сегментных регистров */
        case 0x06: x86_push(cpu, cpu->sregs[X86_ES]); break;
        case 0x07: cpu->sregs[X86_ES] = x86_pop(cpu); break;
        case 0x0E: x86_push(cpu, cpu->sregs[X86_CS]); break;
        case 0x16: x86_push(cpu, cpu->sregs[X86_SS]); break;
        case 0x17: cpu->sregs[X86_SS] = x86_pop(cpu); break;
        case 0x1E: x86_push(cpu, cpu->sregs[X86_DS]); break;
        case 0x1F: cpu->sregs[X86_DS] = x86_pop(cpu); break;

        /* ASCII-коррекции */
        case 0x27: case 0x2F: case 0x37: case 0x3F: {
            uint8_t al = get_reg8(cpu, 0);
            uint8_t ah = get_reg8(cpu, 4);
            if (opcode == 0x27 || opcode == 0x2F) {
                const int adjust = (al & 0x0Fu) > 9 || flag(cpu, X86_AF);
                al = (uint8_t)(al + (adjust ? 6 : 0));
                ah = (uint8_t)(ah + (adjust ? 1 : 0));
            } else {
                const int adjust = (al & 0x0Fu) > 9 || flag(cpu, X86_AF);
                al = (uint8_t)((al + (adjust ? 6 : 0)) & 0x0Fu);
                ah = (uint8_t)(ah + (adjust ? 1 : 0));
            }
            set_reg8(cpu, 0, al);
            set_reg8(cpu, 4, ah);
            break;
        }

        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            cpu->regs[opcode & 7] = inc_dec(cpu, cpu->regs[opcode & 7], 16, 1);
            break;
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            cpu->regs[opcode & 7] = inc_dec(cpu, cpu->regs[opcode & 7], 16, 0);
            break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            x86_push(cpu, cpu->regs[opcode & 7]);
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            cpu->regs[opcode & 7] = x86_pop(cpu);
            break;

        case 0x60: { /* PUSHA */
            const uint16_t sp = cpu->regs[X86_SP];
            x86_push(cpu, cpu->regs[X86_AX]);
            x86_push(cpu, cpu->regs[X86_CX]);
            x86_push(cpu, cpu->regs[X86_DX]);
            x86_push(cpu, cpu->regs[X86_BX]);
            x86_push(cpu, sp);
            x86_push(cpu, cpu->regs[X86_BP]);
            x86_push(cpu, cpu->regs[X86_SI]);
            x86_push(cpu, cpu->regs[X86_DI]);
            break;
        }
        case 0x61: { /* POPA */
            cpu->regs[X86_DI] = x86_pop(cpu);
            cpu->regs[X86_SI] = x86_pop(cpu);
            cpu->regs[X86_BP] = x86_pop(cpu);
            x86_pop(cpu);
            cpu->regs[X86_BX] = x86_pop(cpu);
            cpu->regs[X86_DX] = x86_pop(cpu);
            cpu->regs[X86_CX] = x86_pop(cpu);
            cpu->regs[X86_AX] = x86_pop(cpu);
            break;
        }
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: { /* INS/OUTS */
            const int width = (opcode & 1u) ? 16 : 8;
            const int delta = width == 8 ? 1 : 2;
            const int step = flag(cpu, X86_DF) ? -delta : delta;
            const uint16_t port = cpu->regs[X86_DX];
            const uint16_t source_segment = segment_override >= 0
                                            ? cpu->sregs[segment_override]
                                            : cpu->sregs[X86_DS];
            uint32_t count = repetition ? cpu->regs[X86_CX] : 1u;
            while (count > 0) {
                if (opcode == 0x6C || opcode == 0x6D) {
                    const uint8_t low = port_in(cpu, port);
                    if (width == 8) {
                        mem_write8(cpu, cpu->sregs[X86_ES], cpu->regs[X86_DI], low);
                    } else {
                        const uint8_t high = port_in(cpu, (uint16_t)(port + 1u));
                        mem_write16(cpu, cpu->sregs[X86_ES], cpu->regs[X86_DI],
                                    (uint16_t)(low | ((uint16_t)high << 8)));
                    }
                    cpu->regs[X86_DI] = (uint16_t)(cpu->regs[X86_DI] + step);
                } else {
                    const uint16_t value = width == 8
                                           ? mem_read8(cpu, source_segment, cpu->regs[X86_SI])
                                           : mem_read16(cpu, source_segment, cpu->regs[X86_SI]);
                    port_out(cpu, port, (uint8_t)value);
                    if (width == 16) {
                        port_out(cpu, (uint16_t)(port + 1u), (uint8_t)(value >> 8));
                    }
                    cpu->regs[X86_SI] = (uint16_t)(cpu->regs[X86_SI] + step);
                }
                --count;
                if (repetition) {
                    cpu->regs[X86_CX] = (uint16_t)(cpu->regs[X86_CX] - 1u);
                }
            }
            break;
        }
        case 0x68: x86_push(cpu, fetch16(cpu)); break;
        case 0x6A: x86_push(cpu, (uint16_t)(int16_t)fetch_s8(cpu)); break;
        case 0x69: case 0x6B: { /* IMUL r16, r/m16, imm */
            const ModRM modrm = decode_modrm(cpu, segment_override);
            const int16_t left = (int16_t)read_rm(cpu, &modrm, 16);
            const int16_t right = opcode == 0x69
                                  ? (int16_t)fetch16(cpu)
                                  : (int16_t)fetch_s8(cpu);
            const int32_t product = (int32_t)left * right;
            cpu->regs[modrm.reg] = (uint16_t)product;
            set_flag(cpu, X86_CF, product > 0x7FFF || product < -0x8000);
            set_flag(cpu, X86_OF, product > 0x7FFF || product < -0x8000);
            break;
        }

        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            const int16_t displacement = fetch_s8(cpu);
            if (condition(cpu, opcode & 0x0F)) {
                cpu->ip = (uint16_t)(cpu->ip + displacement);
            }
            break;
        }

        case 0x80: case 0x81: case 0x82: case 0x83: {
            const int width = (opcode & 1u) ? 16 : 8;
            const ModRM modrm = decode_modrm(cpu, segment_override);
            uint16_t immediate;
            if (opcode == 0x81) {
                immediate = fetch16(cpu);
            } else if (opcode == 0x83) {
                immediate = (uint16_t)(int16_t)fetch_s8(cpu);
            } else {
                immediate = fetch8(cpu);
            }
            const uint16_t destination = read_rm(cpu, &modrm, width);
            const uint16_t result = alu(cpu, modrm.reg, destination, immediate, width);
            if (modrm.reg != 7) {
                write_rm(cpu, &modrm, width, result);
            }
            break;
        }
        case 0x84: case 0x85: {
            const int width = (opcode & 1u) ? 16 : 8;
            const ModRM modrm = decode_modrm(cpu, segment_override);
            const uint16_t source = width == 8 ? get_reg8(cpu, modrm.reg) : cpu->regs[modrm.reg];
            alu(cpu, 4, read_rm(cpu, &modrm, width), source, width);
            break;
        }
        case 0x86: case 0x87: {
            const int width = (opcode & 1u) ? 16 : 8;
            const ModRM modrm = decode_modrm(cpu, segment_override);
            const uint16_t left = read_rm(cpu, &modrm, width);
            const uint16_t right = width == 8 ? get_reg8(cpu, modrm.reg) : cpu->regs[modrm.reg];
            write_rm(cpu, &modrm, width, right);
            if (width == 8) {
                set_reg8(cpu, modrm.reg, (uint8_t)left);
            } else {
                cpu->regs[modrm.reg] = left;
            }
            break;
        }
        case 0x88: case 0x89: case 0x8A: case 0x8B: {
            const int width = (opcode & 1u) ? 16 : 8;
            const int direction = opcode & 0x02u;
            const ModRM modrm = decode_modrm(cpu, segment_override);
            if (direction) {
                const uint16_t value = read_rm(cpu, &modrm, width);
                if (width == 8) {
                    set_reg8(cpu, modrm.reg, (uint8_t)value);
                } else {
                    cpu->regs[modrm.reg] = value;
                }
            } else {
                const uint16_t value = width == 8 ? get_reg8(cpu, modrm.reg) : cpu->regs[modrm.reg];
                write_rm(cpu, &modrm, width, value);
            }
            break;
        }
        case 0x8C: case 0x8E: {
            const ModRM modrm = decode_modrm(cpu, segment_override);
            const int index = modrm.reg & 3;
            if (opcode == 0x8C) {
                write_rm(cpu, &modrm, 16, cpu->sregs[index]);
            } else {
                cpu->sregs[index] = read_rm(cpu, &modrm, 16);
            }
            break;
        }
        case 0x8D: {
            const ModRM modrm = decode_modrm(cpu, segment_override);
            cpu->regs[modrm.reg] = modrm.offset;
            break;
        }
        case 0x8F: {
            const ModRM modrm = decode_modrm(cpu, segment_override);
            write_rm(cpu, &modrm, 16, x86_pop(cpu));
            break;
        }
        case 0x90: break; /* NOP / XCHG AX, AX */
        case 0x91: case 0x92: case 0x93: case 0x94:
        case 0x95: case 0x96: case 0x97: {
            const uint16_t temporary = cpu->regs[X86_AX];
            cpu->regs[X86_AX] = cpu->regs[opcode & 7];
            cpu->regs[opcode & 7] = temporary;
            break;
        }
        case 0x98: /* CBW */
            set_reg8(cpu, 4, (get_reg8(cpu, 0) & 0x80u) ? 0xFFu : 0x00u);
            break;
        case 0x99: /* CWD */
            cpu->regs[X86_DX] = (cpu->regs[X86_AX] & 0x8000u) ? 0xFFFFu : 0x0000u;
            break;
        case 0x9A: { /* CALL ptr16:16 */
            const uint16_t new_ip = fetch16(cpu);
            const uint16_t new_cs = fetch16(cpu);
            x86_push(cpu, cpu->sregs[X86_CS]);
            x86_push(cpu, cpu->ip);
            cpu->ip = new_ip;
            cpu->sregs[X86_CS] = new_cs;
            break;
        }
        case 0x9B: break; /* WAIT */
        case 0x9C: x86_push(cpu, cpu->flags); break;
        case 0x9D: cpu->flags = x86_pop(cpu) | 0x0002u; break;
        case 0x9E: /* SAHF */
            cpu->flags = (uint16_t)((cpu->flags & 0xFF00u) | get_reg8(cpu, 4));
            break;
        case 0x9F: set_reg8(cpu, 4, (uint8_t)(cpu->flags & 0xFFu)); break;

        case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
            const int width = (opcode & 1u) ? 16 : 8;
            const uint16_t offset = fetch16(cpu);
            const uint16_t segment = segment_override >= 0
                                     ? cpu->sregs[segment_override]
                                     : cpu->sregs[X86_DS];
            if (opcode <= 0xA1) {
                const uint16_t value = width == 8
                                       ? mem_read8(cpu, segment, offset)
                                       : mem_read16(cpu, segment, offset);
                if (width == 8) {
                    set_reg8(cpu, 0, (uint8_t)value);
                } else {
                    cpu->regs[X86_AX] = value;
                }
            } else {
                const uint16_t value = width == 8 ? get_reg8(cpu, 0) : cpu->regs[X86_AX];
                if (width == 8) {
                    mem_write8(cpu, segment, offset, (uint8_t)value);
                } else {
                    mem_write16(cpu, segment, offset, value);
                }
            }
            break;
        }
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        case 0xAE: case 0xAF:
            string_operation(cpu, opcode, repetition, segment_override);
            break;
        case 0xA8: alu(cpu, 4, get_reg8(cpu, 0), fetch8(cpu), 8); break;
        case 0xA9: alu(cpu, 4, cpu->regs[X86_AX], fetch16(cpu), 16); break;

        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            set_reg8(cpu, opcode & 7, fetch8(cpu));
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            cpu->regs[opcode & 7] = fetch16(cpu);
            break;

        case 0xC0: case 0xC1:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            const int width = (opcode & 1u) ? 16 : 8;
            const ModRM modrm = decode_modrm(cpu, segment_override);
            uint8_t count;
            if (opcode == 0xC0 || opcode == 0xC1) {
                count = fetch8(cpu);
            } else if (opcode == 0xD2 || opcode == 0xD3) {
                count = get_reg8(cpu, 1);
            } else {
                count = 1;
            }
            const uint16_t value = read_rm(cpu, &modrm, width);
            write_rm(cpu, &modrm, width, shift(cpu, modrm.reg, value, count, width));
            break;
        }
        case 0xC2: {
            const uint16_t cleanup = fetch16(cpu);
            cpu->ip = x86_pop(cpu);
            cpu->regs[X86_SP] = (uint16_t)(cpu->regs[X86_SP] + cleanup);
            break;
        }
        case 0xC3: cpu->ip = x86_pop(cpu); break;
        case 0xC4: case 0xC5: { /* LES / LDS */
            const ModRM modrm = decode_modrm(cpu, segment_override);
            cpu->regs[modrm.reg] = x86_read_word(cpu, x86_physical(modrm.segment, modrm.offset));
            const uint16_t segment_value = x86_read_word(
                cpu, x86_physical(modrm.segment, (uint16_t)(modrm.offset + 2u)));
            cpu->sregs[opcode == 0xC4 ? X86_ES : X86_DS] = segment_value;
            break;
        }
        case 0xC6: case 0xC7: {
            const int width = (opcode & 1u) ? 16 : 8;
            const ModRM modrm = decode_modrm(cpu, segment_override);
            write_rm(cpu, &modrm, width, width == 8 ? fetch8(cpu) : fetch16(cpu));
            break;
        }
        case 0xC8: { /* ENTER (186) */
            const uint16_t allocation = fetch16(cpu);
            const uint8_t nesting = fetch8(cpu) & 0x1Fu;
            x86_push(cpu, cpu->regs[X86_BP]);
            const uint16_t frame = cpu->regs[X86_SP];
            for (uint8_t level = 1; level < nesting; ++level) {
                cpu->regs[X86_BP] = (uint16_t)(cpu->regs[X86_BP] - 2u);
                x86_push(cpu, mem_read16(cpu, cpu->sregs[X86_SS], cpu->regs[X86_BP]));
            }
            if (nesting > 0) {
                x86_push(cpu, frame);
            }
            cpu->regs[X86_BP] = frame;
            cpu->regs[X86_SP] = (uint16_t)(cpu->regs[X86_SP] - allocation);
            break;
        }
        case 0xC9: /* LEAVE */
            cpu->regs[X86_SP] = cpu->regs[X86_BP];
            cpu->regs[X86_BP] = x86_pop(cpu);
            break;
        case 0xCA: {
            const uint16_t cleanup = fetch16(cpu);
            cpu->ip = x86_pop(cpu);
            cpu->sregs[X86_CS] = x86_pop(cpu);
            cpu->regs[X86_SP] = (uint16_t)(cpu->regs[X86_SP] + cleanup);
            break;
        }
        case 0xCB:
            cpu->ip = x86_pop(cpu);
            cpu->sregs[X86_CS] = x86_pop(cpu);
            break;
        case 0xCC: x86_raise_interrupt(cpu, 3); break;
        case 0xCD: x86_raise_interrupt(cpu, fetch8(cpu)); break;
        case 0xCE:
            if (flag(cpu, X86_OF)) {
                x86_raise_interrupt(cpu, 4);
            }
            break;
        case 0xCF:
            cpu->ip = x86_pop(cpu);
            cpu->sregs[X86_CS] = x86_pop(cpu);
            cpu->flags = x86_pop(cpu) | 0x0002u;
            break;

        case 0xD4: { /* AAM */
            const uint8_t base = fetch8(cpu);
            if (base == 0) {
                cpu->fault = 1;
                cpu->fault_opcode = opcode;
            } else {
                const uint8_t al = get_reg8(cpu, 0);
                set_reg8(cpu, 4, (uint8_t)(al / base));
                set_reg8(cpu, 0, (uint8_t)(al % base));
                set_flag(cpu, X86_ZF, (al % base) == 0);
                set_flag(cpu, X86_SF, 0);
                set_flag(cpu, X86_PF, parity((uint8_t)(al % base)));
            }
            break;
        }
        case 0xD5: { /* AAD */
            const uint8_t base = fetch8(cpu);
            const uint8_t al = (uint8_t)(get_reg8(cpu, 4) * base + get_reg8(cpu, 0));
            set_reg8(cpu, 0, al);
            set_reg8(cpu, 4, 0);
            set_flag(cpu, X86_ZF, al == 0);
            set_flag(cpu, X86_SF, (al & 0x80u) != 0);
            set_flag(cpu, X86_PF, parity(al));
            break;
        }
        case 0xD7: /* XLAT */
            set_reg8(cpu, 0, mem_read8(cpu,
                     segment_override >= 0 ? cpu->sregs[segment_override] : cpu->sregs[X86_DS],
                     (uint16_t)(cpu->regs[X86_BX] + get_reg8(cpu, 0))));
            break;
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            decode_modrm(cpu, segment_override); /* ESC: сопроцессор игнорируется */
            break;

        case 0xE0: case 0xE1: case 0xE2: {
            const int16_t displacement = fetch_s8(cpu);
            cpu->regs[X86_CX] = (uint16_t)(cpu->regs[X86_CX] - 1u);
            int take = cpu->regs[X86_CX] != 0;
            if (opcode == 0xE0) {
                take = take && !flag(cpu, X86_ZF);
            } else if (opcode == 0xE1) {
                take = take && flag(cpu, X86_ZF);
            }
            if (take) {
                cpu->ip = (uint16_t)(cpu->ip + displacement);
            }
            break;
        }
        case 0xE3: {
            const int16_t displacement = fetch_s8(cpu);
            if (cpu->regs[X86_CX] == 0) {
                cpu->ip = (uint16_t)(cpu->ip + displacement);
            }
            break;
        }
        case 0xE4: set_reg8(cpu, 0, port_in(cpu, fetch8(cpu))); break;
        case 0xE5: cpu->regs[X86_AX] = port_in(cpu, fetch8(cpu)); break;
        case 0xE6: port_out(cpu, fetch8(cpu), get_reg8(cpu, 0)); break;
        case 0xE7: {
            const uint8_t port = fetch8(cpu);
            port_out(cpu, port, get_reg8(cpu, 0));
            port_out(cpu, (uint16_t)(port + 1u), get_reg8(cpu, 4));
            break;
        }
        case 0xE8: {
            const int16_t displacement = fetch_s16(cpu);
            x86_push(cpu, cpu->ip);
            cpu->ip = (uint16_t)(cpu->ip + displacement);
            break;
        }
        case 0xE9: {
            /* Сначала фиксируем смещение: чтение ip и вызов fetch в одном
             * выражении допускает неопределённый порядок и сдвиг на один байт. */
            const int16_t displacement = fetch_s16(cpu);
            cpu->ip = (uint16_t)(cpu->ip + displacement);
            break;
        }
        case 0xEA: {
            const uint16_t new_ip = fetch16(cpu);
            const uint16_t new_cs = fetch16(cpu);
            cpu->ip = new_ip;
            cpu->sregs[X86_CS] = new_cs;
            break;
        }
        case 0xEB: {
            const int16_t displacement = fetch_s8(cpu);
            cpu->ip = (uint16_t)(cpu->ip + displacement);
            break;
        }
        case 0xEC: set_reg8(cpu, 0, port_in(cpu, cpu->regs[X86_DX])); break;
        case 0xED: cpu->regs[X86_AX] = port_in(cpu, cpu->regs[X86_DX]); break;
        case 0xEE: port_out(cpu, cpu->regs[X86_DX], get_reg8(cpu, 0)); break;
        case 0xEF: {
            port_out(cpu, cpu->regs[X86_DX], get_reg8(cpu, 0));
            port_out(cpu, (uint16_t)(cpu->regs[X86_DX] + 1u), get_reg8(cpu, 4));
            break;
        }

        case 0xF4: cpu->halted = 1; break;
        case 0xF5: set_flag(cpu, X86_CF, !flag(cpu, X86_CF)); break;
        case 0xF6: case 0xF7: {
            const int width = (opcode & 1u) ? 16 : 8;
            const uint16_t mask = width == 8 ? 0x00FFu : 0xFFFFu;
            const uint16_t sign = width == 8 ? 0x0080u : 0x8000u;
            const ModRM modrm = decode_modrm(cpu, segment_override);
            uint16_t value = read_rm(cpu, &modrm, width);
            switch (modrm.reg) {
                case 0: case 1: { /* TEST */
                    const uint16_t immediate = width == 8 ? fetch8(cpu) : fetch16(cpu);
                    alu(cpu, 4, value, immediate, width);
                    break;
                }
                case 2: value = (uint16_t)(~value & mask); write_rm(cpu, &modrm, width, value); break;
                case 3: value = alu(cpu, 5, 0, value, width); write_rm(cpu, &modrm, width, value); break;
                case 4: case 5: { /* MUL / IMUL */
                    const uint32_t left = width == 8 ? get_reg8(cpu, 0) : cpu->regs[X86_AX];
                    if (modrm.reg == 4) {
                        const uint32_t product = left * value;
                        if (width == 8) {
                            cpu->regs[X86_AX] = (uint16_t)product;
                            set_flag(cpu, X86_CF, product > 0xFFu);
                            set_flag(cpu, X86_OF, product > 0xFFu);
                        } else {
                            cpu->regs[X86_AX] = (uint16_t)product;
                            cpu->regs[X86_DX] = (uint16_t)(product >> 16);
                            set_flag(cpu, X86_CF, cpu->regs[X86_DX] != 0);
                            set_flag(cpu, X86_OF, cpu->regs[X86_DX] != 0);
                        }
                    } else {
                        int32_t a;
                        int32_t b;
                        if (width == 8) {
                            a = (int32_t)(int8_t)(uint8_t)left;
                            b = (int32_t)(int8_t)(uint8_t)value;
                        } else {
                            a = (int32_t)(int16_t)(uint16_t)left;
                            b = (int32_t)(int16_t)(uint16_t)value;
                        }
                        const int32_t product = a * b;
                        const uint16_t low = (uint16_t)product;
                        if (width == 8) {
                            cpu->regs[X86_AX] = (uint16_t)(product & 0xFFFFu);
                            set_flag(cpu, X86_CF, product != (int8_t)product);
                            set_flag(cpu, X86_OF, product != (int8_t)product);
                        } else {
                            cpu->regs[X86_AX] = low;
                            cpu->regs[X86_DX] = (uint16_t)(product >> 16);
                            set_flag(cpu, X86_CF, product != (int16_t)product);
                            set_flag(cpu, X86_OF, product != (int16_t)product);
                        }
                    }
                    break;
                }
                default: { /* DIV / IDIV */
                    const uint32_t dividend = width == 8
                                             ? cpu->regs[X86_AX]
                                             : (cpu->regs[X86_AX] | ((uint32_t)cpu->regs[X86_DX] << 16));
                    if (value == 0) {
                        x86_raise_interrupt(cpu, 0);
                        break;
                    }
                    const uint32_t quotient = dividend / value;
                    const uint32_t remainder = dividend % value;
                    if (quotient > mask) {
                        x86_raise_interrupt(cpu, 0);
                        break;
                    }
                    if (width == 8) {
                        set_reg8(cpu, 0, (uint8_t)quotient);
                        set_reg8(cpu, 4, (uint8_t)remainder);
                    } else {
                        cpu->regs[X86_AX] = (uint16_t)quotient;
                        cpu->regs[X86_DX] = (uint16_t)remainder;
                    }
                    break;
                }
            }
            (void)sign;
            break;
        }
        case 0xF8: set_flag(cpu, X86_CF, 0); break;
        case 0xF9: set_flag(cpu, X86_CF, 1); break;
        case 0xFA: set_flag(cpu, X86_IF, 0); break;
        case 0xFB: set_flag(cpu, X86_IF, 1); break;
        case 0xFC: set_flag(cpu, X86_DF, 0); break;
        case 0xFD: set_flag(cpu, X86_DF, 1); break;
        case 0xFE: {
            const ModRM modrm = decode_modrm(cpu, segment_override);
            write_rm(cpu, &modrm, 8,
                     inc_dec(cpu, read_rm(cpu, &modrm, 8), 8, modrm.reg == 0));
            break;
        }
        case 0xFF: {
            const ModRM modrm = decode_modrm(cpu, segment_override);
            const uint16_t value = read_rm(cpu, &modrm, 16);
            switch (modrm.reg) {
                case 0: write_rm(cpu, &modrm, 16, inc_dec(cpu, value, 16, 1)); break;
                case 1: write_rm(cpu, &modrm, 16, inc_dec(cpu, value, 16, 0)); break;
                case 2: x86_push(cpu, cpu->ip); cpu->ip = value; break;
                case 3: {
                    const uint32_t address = x86_physical(modrm.segment, modrm.offset);
                    const uint16_t new_ip = x86_read_word(cpu, address);
                    const uint16_t new_cs = x86_read_word(cpu, address + 2u);
                    x86_push(cpu, cpu->sregs[X86_CS]);
                    x86_push(cpu, cpu->ip);
                    cpu->ip = new_ip;
                    cpu->sregs[X86_CS] = new_cs;
                    break;
                }
                case 4: cpu->ip = value; break;
                case 5: {
                    const uint32_t address = x86_physical(modrm.segment, modrm.offset);
                    cpu->ip = x86_read_word(cpu, address);
                    cpu->sregs[X86_CS] = x86_read_word(cpu, address + 2u);
                    break;
                }
                default: x86_push(cpu, value); break;
            }
            break;
        }

        default:
            cpu->fault = 1;
            cpu->fault_opcode = opcode;
            cpu->ip = start_ip;
            return 0;
    }

    /* INT 16h AH=00 пустой очередь обрабатывается firmware: ожидание ввода. */
    if (cpu->waiting) {
        cpu->ip = start_ip;
        return 0;
    }
    return 1;
}
