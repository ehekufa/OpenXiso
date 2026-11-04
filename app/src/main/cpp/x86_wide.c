/*
 * x86_wide.c — широкое ядро интерпретатора: 32-битный защищённый режим i386
 * и 64-битный long mode x86-64.
 *
 * Один декодер обслуживает и 16-битные инструкции с префиксом 0x66/0x67
 * (переходная стадия загрузчика), и полноценный PM/LM. Реализованы: ModRM/SIB,
 * RIP-относительная адресация, REX-префиксы, 8/16/32/64-битные ALU, MOV/LEA,
 * PUSH/POP, CALL/RET/JMP/Jcc, LGDT/LIDT/SGDT/SIDT, MOV CR0/CR3/CR4,
 * RDMSR/WRMSR (EFER), far jmp по дескриптору GDT и CPUID.
 *
 * Машина всегда стартует в реальном режиме; гость сам включает PE в CR0 и
 * LME в EFER. Предельная разрядность задаётся полем max_mode — это и есть
 * выбор «16 / 32 / 64 бита» в интерфейсе приложения.
 */

#include "include/x86_cpu.h"

#include <string.h>

/*
 * Никаких __int128: NDK собирает и 32-битные ABI (armeabi-v7a, x86), где этот
 * тип недоступен. Перенос/заём вычисляются явно на uint64_t.
 */

/* ---------------- Служебные константы и флаги ---------------- */

static uint64_t width_mask(int width) {
    switch (width) {
        case 8:  return 0xFFu;
        case 16: return 0xFFFFu;
        case 32: return 0xFFFFFFFFu;
        default: return UINT64_MAX;
    }
}

static void set_flag(X86Cpu* cpu, uint16_t mask, int value) {
    if (value) {
        cpu->flags |= mask;
    } else {
        cpu->flags &= (uint16_t)~mask;
    }
}

static int flag(const X86Cpu* cpu, uint16_t mask) {
    return (cpu->flags & mask) != 0;
}

static int parity(uint8_t value) {
    int bits = 0;
    for (int index = 0; index < 8; ++index) {
        bits += (value >> index) & 1;
    }
    return (bits & 1) == 0;
}

const char* x86_mode_name(X86Mode mode) {
    switch (mode) {
        case X86_MODE_32: return "32 бита";
        case X86_MODE_64: return "64 бита";
        default: return "16 бит";
    }
}

const char* x86_mode_description(X86Mode mode) {
    switch (mode) {
        case X86_MODE_32: return "i386, защищённый режим";
        case X86_MODE_64: return "x86-64, long mode";
        default: return "8086/186, реальный режим";
    }
}

void x86_set_max_mode(X86Cpu* cpu, X86Mode mode) {
    if (cpu == NULL) {
        return;
    }
    cpu->max_mode = (mode == X86_MODE_64) ? X86_MODE_64
                    : (mode == X86_MODE_32) ? X86_MODE_32 : X86_MODE_16;
}

X86Mode x86_get_mode(const X86Cpu* cpu) {
    return cpu == NULL ? X86_MODE_16 : cpu->mode;
}

X86Mode x86_get_max_mode(const X86Cpu* cpu) {
    return cpu == NULL ? X86_MODE_16 : cpu->max_mode;
}

uint64_t x86_wide_ip(const X86Cpu* cpu) {
    if (cpu == NULL) {
        return 0;
    }
    return cpu->mode == X86_MODE_16 ? (uint64_t)cpu->ip : cpu->rip;
}

void x86_wide_sync_from_real(X86Cpu* cpu) {
    if (cpu == NULL) {
        return;
    }
    for (int index = 0; index < 8; ++index) {
        cpu->gpr[index] = (cpu->gpr[index] & ~UINT64_C(0xFFFF))
                          | (uint64_t)cpu->regs[index];
    }
}

void x86_wide_sync_to_real(X86Cpu* cpu) {
    if (cpu == NULL) {
        return;
    }
    for (int index = 0; index < 8; ++index) {
        cpu->regs[index] = (uint16_t)cpu->gpr[index];
    }
}

static void wide_initialize(X86Cpu* cpu) {
    if (cpu->wide_ready) {
        return;
    }
    for (int index = 0; index < 8; ++index) {
        cpu->gpr[index] = (uint64_t)cpu->regs[index];
    }
    for (int index = 0; index < 6; ++index) {
        cpu->seg_sel[index] = cpu->sregs[index];
        cpu->seg_base[index] = (uint64_t)cpu->sregs[index] * 16u;
        cpu->seg_limit[index] = 0xFFFFu;
    }
    cpu->rip = (uint64_t)cpu->ip;
    cpu->cr0 = 0u;
    cpu->efer = 0u;
    cpu->wide_ready = 1;
}

/* ---------------- Память ---------------- */

static uint64_t linearize(const X86Cpu* cpu, uint64_t offset, int segment) {
    uint64_t address = cpu->seg_base[segment] + offset;
    if (cpu->mode == X86_MODE_16 && !cpu->a20_enabled) {
        address &= 0xFFFFFu;
    }
    return address;
}

static uint64_t read_mem(const X86Cpu* cpu, uint64_t address, int width) {
    const size_t mask = cpu->memory_size - 1u;
    uint64_t value = 0;
    for (int index = 0; index < width / 8; ++index) {
        const size_t position = (size_t)((address + (uint64_t)index) & (uint64_t)mask);
        value |= (uint64_t)cpu->memory[position] << (8 * index);
    }
    return value;
}

static void write_mem(X86Cpu* cpu, uint64_t address, int width, uint64_t value) {
    const size_t mask = cpu->memory_size - 1u;
    for (int index = 0; index < width / 8; ++index) {
        const size_t position = (size_t)((address + (uint64_t)index) & (uint64_t)mask);
        cpu->memory[position] = (uint8_t)(value >> (8 * index));
    }
}

static uint8_t fetch8(X86Cpu* cpu) {
    const uint64_t address = linearize(cpu, cpu->rip, X86_CS);
    const uint8_t value = (uint8_t)read_mem(cpu, address, 8);
    cpu->rip += 1u;
    return value;
}

static uint16_t fetch16(X86Cpu* cpu) {
    const uint16_t low = fetch8(cpu);
    const uint16_t high = fetch8(cpu);
    return (uint16_t)(low | (high << 8));
}

static uint32_t fetch32(X86Cpu* cpu) {
    const uint32_t b0 = fetch8(cpu);
    const uint32_t b1 = fetch8(cpu);
    const uint32_t b2 = fetch8(cpu);
    const uint32_t b3 = fetch8(cpu);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static uint64_t fetch64(X86Cpu* cpu) {
    const uint64_t low = fetch32(cpu);
    const uint64_t high = fetch32(cpu);
    return low | (high << 32);
}

static int8_t fetch_s8(X86Cpu* cpu) { return (int8_t)fetch8(cpu); }
static int16_t fetch_s16(X86Cpu* cpu) { return (int16_t)fetch16(cpu); }
static int32_t fetch_s32(X86Cpu* cpu) { return (int32_t)fetch32(cpu); }

/* ---------------- Регистры ---------------- */

static uint64_t get_gpr(const X86Cpu* cpu, int index, int width) {
    return cpu->gpr[index & 15] & width_mask(width);
}

static void set_gpr(X86Cpu* cpu, int index, int width, uint64_t value) {
    const uint64_t mask = width_mask(width);
    if (width == 64) {
        cpu->gpr[index & 15] = value & mask;
    } else if (width == 32) {
        /* Запись в 32-битный регистр обнуляет старшую половину (x86-64). */
        cpu->gpr[index & 15] = value & 0xFFFFFFFFu;
    } else {
        cpu->gpr[index & 15] = (cpu->gpr[index & 15] & ~mask) | (value & mask);
    }
    x86_wide_sync_to_real(cpu);
}

static uint8_t get_reg8(const X86Cpu* cpu, int index, int rex) {
    if (!rex && index >= 4 && index <= 7) {
        return (uint8_t)(cpu->gpr[index - 4] >> 8);
    }
    return (uint8_t)cpu->gpr[index & 15];
}

static void set_reg8(X86Cpu* cpu, int index, int rex, uint8_t value) {
    if (!rex && index >= 4 && index <= 7) {
        cpu->gpr[index - 4] = (cpu->gpr[index - 4] & ~UINT64_C(0xFF00))
                              | ((uint64_t)value << 8);
    } else {
        cpu->gpr[index & 15] = (cpu->gpr[index & 15] & ~UINT64_C(0xFF)) | value;
    }
    x86_wide_sync_to_real(cpu);
}

/* ---------------- ALU ---------------- */

/** Перенос сложения без 128-битного типа. */
static int addition_carry(uint64_t a, uint64_t b, uint64_t carry_in, int width) {
    if (width != 64) {
        /* a и b уже обрезаны по маске, переполнения uint64_t здесь быть не может. */
        return ((a + b + carry_in) >> width) != 0;
    }
    const uint64_t partial = a + b;
    if (partial < a) {
        return 1;
    }
    return carry_in != 0 && partial == UINT64_MAX;
}

/** Заём вычитания без переполнения при b + carry_in. */
static int subtraction_borrow(uint64_t a, uint64_t b, uint64_t carry_in) {
    return a < b || (carry_in != 0 && a == b);
}

static uint64_t wide_alu(X86Cpu* cpu, int operation, uint64_t a, uint64_t b,
                         int width) {
    const uint64_t mask = width_mask(width);
    const uint64_t sign = UINT64_C(1) << (width - 1);
    a &= mask;
    b &= mask;
    const uint64_t carry_in = (operation == 2 || operation == 3) && flag(cpu, X86_CF)
                              ? 1u : 0u;
    uint64_t full = 0;
    int carry = 0;

    switch (operation) {
        case 0:                                             /* ADD */
            full = a + b;
            carry = addition_carry(a, b, 0u, width);
            break;
        case 1:                                             /* OR  */
            full = a | b;
            break;
        case 2:                                             /* ADC */
            full = a + b + carry_in;
            carry = addition_carry(a, b, carry_in, width);
            break;
        case 3:                                             /* SBB */
            full = a - b - carry_in;
            carry = subtraction_borrow(a, b, carry_in);
            break;
        case 4:                                             /* AND */
            full = a & b;
            break;
        case 5:                                             /* SUB */
            full = a - b;
            carry = subtraction_borrow(a, b, 0u);
            break;
        case 6:                                             /* XOR */
            full = a ^ b;
            break;
        default:                                            /* CMP */
            full = a - b;
            carry = subtraction_borrow(a, b, 0u);
            break;
    }

    /* 2^64 кратно 2^width, поэтому обрезание даёт верный результат по модулю. */
    const uint64_t result = full & mask;
    if (operation == 1 || operation == 4 || operation == 6) {
        set_flag(cpu, X86_CF, 0);
        set_flag(cpu, X86_OF, 0);
    } else {
        set_flag(cpu, X86_CF, carry);
        int overflow;
        if (operation == 0 || operation == 2) {
            overflow = ((~(a ^ b) & (a ^ result)) & sign) != 0;
        } else {
            overflow = (((a ^ b) & (a ^ result)) & sign) != 0;
        }
        set_flag(cpu, X86_OF, overflow);
    }
    set_flag(cpu, X86_AF, ((a ^ b ^ result) & 0x10u) != 0);
    set_flag(cpu, X86_ZF, result == 0);
    set_flag(cpu, X86_SF, (result & sign) != 0);
    set_flag(cpu, X86_PF, parity((uint8_t)result));
    return result;
}

/* ---------------- ModRM / SIB ---------------- */

typedef struct WideModRM {
    int mod;
    int reg;
    int rm;
    int is_register;
    int segment;
    int rex_r;
    int rex_x;
    int rex_b;
    int rex_w;
    int rex_present;
    uint64_t offset;      /* смещение внутри сегмента */
    int rip_relative;
    int64_t displacement;
} WideModRM;

static uint64_t modrm_address(const X86Cpu* cpu, const WideModRM* modrm) {
    const uint64_t offset = modrm->rip_relative
                            ? (uint64_t)((int64_t)cpu->rip + modrm->displacement)
                            : modrm->offset;
    return linearize(cpu, offset, modrm->segment);
}

static WideModRM decode_modrm(X86Cpu* cpu, int address_width, int segment,
                              int rex_r, int rex_x, int rex_b, int rex_w,
                              int rex_present) {
    WideModRM result;
    memset(&result, 0, sizeof(result));
    result.segment = segment;
    result.rex_r = rex_r;
    result.rex_x = rex_x;
    result.rex_b = rex_b;
    result.rex_w = rex_w;
    result.rex_present = rex_present;

    const uint8_t byte = fetch8(cpu);
    result.mod = byte >> 6;
    result.reg = ((byte >> 3) & 7) | (rex_r ? 8 : 0);
    result.rm = (byte & 7) | ((result.mod == 3 && rex_b) ? 8 : 0);
    if (result.mod == 3) {
        result.is_register = 1;
        return result;
    }

    if (address_width == 16) {
        uint16_t base = 0;
        switch (byte & 7) {
            case 0: base = (uint16_t)(get_gpr(cpu, X86_RBX, 16) + get_gpr(cpu, X86_RSI, 16)); break;
            case 1: base = (uint16_t)(get_gpr(cpu, X86_RBX, 16) + get_gpr(cpu, X86_RDI, 16)); break;
            case 2: base = (uint16_t)(get_gpr(cpu, X86_RBP, 16) + get_gpr(cpu, X86_RSI, 16)); break;
            case 3: base = (uint16_t)(get_gpr(cpu, X86_RBP, 16) + get_gpr(cpu, X86_RDI, 16)); break;
            case 4: base = (uint16_t)get_gpr(cpu, X86_RSI, 16); break;
            case 5: base = (uint16_t)get_gpr(cpu, X86_RDI, 16); break;
            case 6: base = (uint16_t)get_gpr(cpu, X86_RBP, 16); break;
            default: base = (uint16_t)get_gpr(cpu, X86_RBX, 16); break;
        }
        int64_t displacement = 0;
        if (result.mod == 0 && (byte & 7) == 6) {
            displacement = fetch_s16(cpu);
            base = 0;
        } else if (result.mod == 1) {
            displacement = fetch_s8(cpu);
        } else if (result.mod == 2) {
            displacement = fetch_s16(cpu);
        }
        result.offset = (uint64_t)(int64_t)base + (uint64_t)displacement;
        return result;
    }

    int base_register = (byte & 7) | (rex_b ? 8 : 0);
    int index_register = -1;
    int scale = 1;
    int64_t displacement = 0;

    if ((byte & 7) == 4) {                       /* есть SIB */
        const uint8_t sib = fetch8(cpu);
        scale = 1 << (sib >> 6);
        const int raw_index = (sib >> 3) & 7;
        index_register = raw_index | (rex_x ? 8 : 0);
        if (raw_index == 4 && !rex_x) {
            index_register = -1;
        }
        base_register = (sib & 7) | (rex_b ? 8 : 0);
        if ((sib & 7) == 5 && result.mod == 0) {
            base_register = -1;
            displacement = fetch_s32(cpu);
        }
    } else if (result.mod == 0 && (byte & 7) == 5) {
        displacement = fetch_s32(cpu);
        if (address_width == 64) {
            result.rip_relative = 1;
            result.displacement = displacement;
            return result;
        }
        base_register = -1;
    }

    if (result.mod == 1) {
        displacement += fetch_s8(cpu);
    } else if (result.mod == 2) {
        displacement += fetch_s32(cpu);
    }

    uint64_t offset = 0;
    if (base_register >= 0) {
        offset = get_gpr(cpu, base_register, address_width);
    }
    if (index_register >= 0) {
        offset += get_gpr(cpu, index_register, address_width) * (uint64_t)scale;
    }
    offset += (uint64_t)displacement;
    if (address_width == 32) {
        offset &= 0xFFFFFFFFu;
    }
    result.offset = offset;
    return result;
}

static uint64_t read_rm(X86Cpu* cpu, const WideModRM* modrm, int width, int rex) {
    if (modrm->is_register) {
        return width == 8 ? get_reg8(cpu, modrm->rm, rex)
                          : get_gpr(cpu, modrm->rm, width);
    }
    return read_mem(cpu, modrm_address(cpu, modrm), width);
}

static void write_rm(X86Cpu* cpu, const WideModRM* modrm, int width, int rex,
                     uint64_t value) {
    if (modrm->is_register) {
        if (width == 8) {
            set_reg8(cpu, modrm->rm, rex, (uint8_t)value);
        } else {
            set_gpr(cpu, modrm->rm, width, value);
        }
        return;
    }
    write_mem(cpu, modrm_address(cpu, modrm), width, value);
}

/* ---------------- Стек и переходы ---------------- */

static void wide_push(X86Cpu* cpu, int width, uint64_t value) {
    const int bytes = width / 8;
    const uint64_t stack = (get_gpr(cpu, X86_RSP, width == 64 ? 64 : 32)
                            - (uint64_t)bytes) & width_mask(width == 64 ? 64 : 32);
    set_gpr(cpu, X86_RSP, width == 64 ? 64 : 32, stack);
    write_mem(cpu, linearize(cpu, stack, X86_SS), width, value);
}

static uint64_t wide_pop(X86Cpu* cpu, int width) {
    const int bytes = width / 8;
    const int pointer_width = width == 64 ? 64 : 32;
    const uint64_t stack = get_gpr(cpu, X86_RSP, pointer_width);
    const uint64_t value = read_mem(cpu, linearize(cpu, stack, X86_SS), width);
    set_gpr(cpu, X86_RSP, pointer_width, (stack + (uint64_t)bytes)
            & width_mask(pointer_width));
    return value;
}

static int condition(const X86Cpu* cpu, int code) {
    switch (code & 0x0F) {
        case 0:  return flag(cpu, X86_OF);
        case 1:  return !flag(cpu, X86_OF);
        case 2:  return flag(cpu, X86_CF);
        case 3:  return !flag(cpu, X86_CF);
        case 4:  return flag(cpu, X86_ZF);
        case 5:  return !flag(cpu, X86_ZF);
        case 6:  return flag(cpu, X86_CF) || flag(cpu, X86_ZF);
        case 7:  return !flag(cpu, X86_CF) && !flag(cpu, X86_ZF);
        case 8:  return flag(cpu, X86_SF);
        case 9:  return !flag(cpu, X86_SF);
        case 10: return flag(cpu, X86_PF);
        case 11: return !flag(cpu, X86_PF);
        case 12: return flag(cpu, X86_SF) != flag(cpu, X86_OF);
        case 13: return flag(cpu, X86_SF) == flag(cpu, X86_OF);
        case 14: return flag(cpu, X86_ZF) || (flag(cpu, X86_SF) != flag(cpu, X86_OF));
        default: return !flag(cpu, X86_ZF) && (flag(cpu, X86_SF) == flag(cpu, X86_OF));
    }
}

/* ---------------- Переключение режимов ---------------- */

static void fault_wide(X86Cpu* cpu, uint8_t opcode, const char* reason) {
    cpu->fault = 1;
    cpu->fault_opcode = opcode;
    cpu->fault_reason = reason;
}

static void apply_cr0(X86Cpu* cpu) {
    if (!(cpu->cr0 & X86_CR0_PE)) {
        cpu->mode = X86_MODE_16;
        return;
    }
    if (cpu->max_mode < X86_MODE_32) {
        fault_wide(cpu, 0x0Fu, "Гостю нужен 32-битный процессор (выбран режим 16 бит)");
        return;
    }
    if ((cpu->cr0 & X86_CR0_PG) && (cpu->efer & X86_EFER_LME)
        && cpu->max_mode < X86_MODE_64) {
        fault_wide(cpu, 0x0Fu,
                   "Гостю нужен 64-битный процессор (выбран режим 32 бита)");
        return;
    }
    /*
     * Разрядность меняется НЕ здесь: до far jmp сегмент CS всё ещё 16-битный,
     * поэтому префикс 0x66 должен давать 32-битное смещение перехода.
     * Режим устанавливает load_descriptor() по битам D/B и L дескриптора GDT.
     */
}

static void load_descriptor(X86Cpu* cpu, uint16_t selector, uint64_t offset,
                            uint8_t opcode) {
    if (cpu->gdtr_base == 0 || selector + 7u > cpu->gdtr_limit) {
        fault_wide(cpu, opcode, "Неверный селектор сегмента в GDT");
        return;
    }
    const uint64_t entry = cpu->gdtr_base + (uint64_t)selector;
    const uint8_t* descriptor = cpu->memory
                                + (size_t)(entry & (uint64_t)(cpu->memory_size - 1u));
    const uint32_t base = (uint32_t)descriptor[2]
                          | ((uint32_t)descriptor[3] << 8)
                          | ((uint32_t)descriptor[4] << 16)
                          | ((uint32_t)descriptor[7] << 24);
    const uint8_t access = descriptor[5];
    const uint8_t flags = descriptor[6];
    const int long_mode = (flags & 0x20u) != 0 && (access & 0x08u) != 0;
    const int default_32 = (flags & 0x40u) != 0;

    if (long_mode && cpu->max_mode < X86_MODE_64) {
        fault_wide(cpu, opcode,
                   "Гостю нужен 64-битный процессор (дескриптор кода с L=1)");
        return;
    }
    if ((default_32 || long_mode) && cpu->max_mode < X86_MODE_32) {
        fault_wide(cpu, opcode,
                   "Гостю нужен 32-битный процессор (выбран режим 16 бит)");
        return;
    }

    cpu->seg_sel[X86_CS] = selector;
    cpu->seg_base[X86_CS] = base;
    cpu->sregs[X86_CS] = selector;
    cpu->seg_limit[X86_CS] = 0xFFFFFFFFu;
    cpu->rip = offset;
    cpu->ip = (uint16_t)offset;
    cpu->mode = long_mode ? X86_MODE_64 : (default_32 ? X86_MODE_32 : X86_MODE_16);
}

static void wide_0f(X86Cpu* cpu, uint8_t subopcode, int width, int address_width,
                    int segment, int rex_r, int rex_x, int rex_b, int rex_w,
                    int rex_present) {
    if (subopcode >= 0x80 && subopcode <= 0x8F) {
        const int32_t displacement = fetch_s32(cpu);
        if (condition(cpu, subopcode & 0x0F)) {
            cpu->rip = (uint64_t)((int64_t)cpu->rip + displacement);
        }
        return;
    }

    switch (subopcode) {
        case 0x01: {                     /* LGDT / LIDT / SGDT / SIDT */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                 rex_r, rex_x, rex_b, rex_w,
                                                 rex_present);
            const uint64_t address = modrm_address(cpu, &modrm);
            if (modrm.reg == 2 || modrm.reg == 3) {
                const uint16_t limit = (uint16_t)read_mem(cpu, address, 16);
                const uint64_t base = read_mem(cpu, address + 2u,
                                               width == 64 ? 64 : 32);
                if (modrm.reg == 2) {
                    cpu->gdtr_limit = limit;
                    cpu->gdtr_base = base;
                } else {
                    cpu->idtr_limit = limit;
                    cpu->idtr_base = base;
                }
            } else if (modrm.reg == 0 || modrm.reg == 1) {
                const uint16_t limit = modrm.reg == 0 ? cpu->gdtr_limit
                                                       : cpu->idtr_limit;
                const uint64_t base = modrm.reg == 0 ? cpu->gdtr_base
                                                      : cpu->idtr_base;
                write_mem(cpu, address, 16, limit);
                write_mem(cpu, address + 2u, width == 64 ? 64 : 32, base);
            }
            return;
        }
        case 0x20:                       /* MOV r32, CRn */
        case 0x22: {                     /* MOV CRn, r32 */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                 rex_r, rex_x, rex_b, rex_w,
                                                 rex_present);
            const uint64_t value = get_gpr(cpu, modrm.rm, 32);
            uint32_t* control = modrm.reg == 0 ? &cpu->cr0
                                : modrm.reg == 3 ? &cpu->cr3
                                : modrm.reg == 4 ? &cpu->cr4 : NULL;
            if (control == NULL) {
                fault_wide(cpu, 0x0Fu, "Неподдерживаемый управляющий регистр CR");
                return;
            }
            if (subopcode == 0x22) {
                if (modrm.reg == 0) {
                    cpu->cr0 = (uint32_t)value;
                    apply_cr0(cpu);
                } else {
                    *control = (uint32_t)value;
                }
            } else {
                set_gpr(cpu, modrm.rm, 32, *control);
            }
            return;
        }
        case 0x30:                       /* WRMSR */
            if ((uint32_t)get_gpr(cpu, X86_RCX, 32) == X86_MSR_EFER) {
                cpu->efer = (get_gpr(cpu, X86_RDX, 32) << 32)
                            | get_gpr(cpu, X86_RAX, 32);
            }
            return;
        case 0x32:                       /* RDMSR */
            if ((uint32_t)get_gpr(cpu, X86_RCX, 32) == X86_MSR_EFER) {
                set_gpr(cpu, X86_RAX, 32, (uint32_t)cpu->efer);
                set_gpr(cpu, X86_RDX, 32, (uint32_t)(cpu->efer >> 32));
            } else {
                set_gpr(cpu, X86_RAX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, 0u);
            }
            return;
        case 0xA2: {                     /* CPUID: минимальный, но честный ответ */
            const uint32_t leaf = (uint32_t)get_gpr(cpu, X86_RAX, 32);
            if (leaf == 0) {
                set_gpr(cpu, X86_RAX, 32, 1u);
                set_gpr(cpu, X86_RBX, 32, 0x756E6547u);   /* Genu */
                set_gpr(cpu, X86_RCX, 32, 0x6C65746Eu);   /* ntel */
                set_gpr(cpu, X86_RDX, 32, 0x49656E69u);   /* ineI */
            } else if (leaf == 1u) {
                set_gpr(cpu, X86_RAX, 32, 0x000006F2u);
                set_gpr(cpu, X86_RBX, 32, 0u);
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, 0u);
            } else if (leaf == 0x80000001u) {
                set_gpr(cpu, X86_RAX, 32, leaf);
                set_gpr(cpu, X86_RBX, 32, 0u);
                set_gpr(cpu, X86_RCX, 32, 0u);
                /* EDX.29 = Long Mode, только если процессор 64-битный. */
                set_gpr(cpu, X86_RDX, 32,
                        cpu->max_mode == X86_MODE_64 ? (1u << 29) : 0u);
            } else {
                set_gpr(cpu, X86_RAX, 32, 0u);
                set_gpr(cpu, X86_RBX, 32, 0u);
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, 0u);
            }
            return;
        }
        case 0xB6:                       /* MOVZX r, r/m8 */
        case 0xB7: {                     /* MOVZX r, r/m16 */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                 rex_r, rex_x, rex_b, rex_w,
                                                 rex_present);
            const uint64_t value = read_rm(cpu, &modrm,
                                           subopcode == 0xB6 ? 8 : 16,
                                           rex_present);
            set_gpr(cpu, modrm.reg, width, value);
            return;
        }
        case 0xAF: {                     /* IMUL r, r/m */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                 rex_r, rex_x, rex_b, rex_w,
                                                 rex_present);
            const uint64_t a = get_gpr(cpu, modrm.reg, width);
            const uint64_t b = read_rm(cpu, &modrm, width, rex_present);
            const uint64_t mask = width_mask(width);
            /* Умножаем как беззнаковые: переполнение int64_t было бы неопределённым. */
            const uint64_t product = a * b;
            const uint64_t result = product & mask;
            set_gpr(cpu, modrm.reg, width, result);
            const int overflow = width == 64 ? 0 : (product >> width) != 0;
            set_flag(cpu, X86_CF, overflow);
            set_flag(cpu, X86_OF, overflow);
            return;
        }
        case 0x05:                       /* SYSCALL: гостю нужен свой обработчик */
            fault_wide(cpu, 0x0Fu, "SYSCALL требует обработчика ядра гостя");
            return;
        case 0x0B:                       /* UD2 */
            fault_wide(cpu, 0x0Fu, "Инструкция UD2: гость запросил останов");
            return;
        default:
            fault_wide(cpu, 0x0Fu, "Неподдерживаемая инструкция 0Fh в широком режиме");
            return;
    }
}

/* ---------------- Одна инструкция широкого ядра ---------------- */

int x86_wide_step(X86Cpu* cpu) {
    if (cpu->halted || cpu->waiting || cpu->fault) {
        return 0;
    }
    wide_initialize(cpu);
    if (cpu->mode == X86_MODE_16) {
        cpu->rip = (uint64_t)cpu->ip;
        /* Сегменты реального режима мог изменить far jmp/BIOS. */
        for (int index = 0; index < 6; ++index) {
            cpu->seg_sel[index] = cpu->sregs[index];
            cpu->seg_base[index] = (uint64_t)cpu->sregs[index] * 16u;
            cpu->seg_limit[index] = 0xFFFFu;
        }
    }
    cpu->instructions += 1u;
    const uint64_t start_rip = cpu->rip;

    int operand_override = 0;
    int address_override = 0;
    int segment = X86_DS;
    int rex_r = 0, rex_x = 0, rex_b = 0, rex_w = 0, rex_present = 0;
    uint8_t opcode = 0;

    for (;;) {
        opcode = fetch8(cpu);
        if (opcode == 0xF0 || opcode == 0xF2 || opcode == 0xF3) {
            continue;
        }
        if (opcode == 0x66) { operand_override = 1; continue; }
        if (opcode == 0x67) { address_override = 1; continue; }
        if (opcode == 0x2E) { segment = X86_CS; continue; }
        if (opcode == 0x36) { segment = X86_SS; continue; }
        if (opcode == 0x3E) { segment = X86_DS; continue; }
        if (opcode == 0x26) { segment = X86_ES; continue; }
        if (opcode == 0x64) { segment = X86_FS; continue; }
        if (opcode == 0x65) { segment = X86_GS; continue; }
        if (cpu->mode == X86_MODE_64 && opcode >= 0x40 && opcode <= 0x4F) {
            rex_present = 1;
            rex_w = (opcode & 0x08u) != 0;
            rex_r = (opcode & 0x04u) != 0;
            rex_x = (opcode & 0x02u) != 0;
            rex_b = (opcode & 0x01u) != 0;
            continue;
        }
        break;
    }

    if (cpu->max_mode == X86_MODE_16 &&
        (operand_override || address_override || opcode == 0x0F)) {
        cpu->rip = (uint64_t)cpu->ip;
        fault_wide(cpu, opcode,
                   "Гостю нужен 32-битный процессор (выбран режим 16 бит)");
        return 0;
    }

    int width = cpu->mode == X86_MODE_16 ? 16 : 32;
    if (operand_override) {
        width = width == 16 ? 32 : 16;
    }
    if (rex_w) {
        width = 64;
    }
    int address_width = cpu->mode == X86_MODE_16 ? 16
                        : cpu->mode == X86_MODE_32 ? 32 : 64;
    if (address_override) {
        address_width = address_width == 16 ? 32
                        : address_width == 32 ? 16 : 32;
    }

    int done = 0;

    /* Групповая арифметика 0x00..0x3D. */
    if (opcode <= 0x3Du && (opcode & 0x07u) <= 0x05u) {
        const int operation = opcode >> 3;
        const int operation_width = width == 64 ? 64 : ((opcode & 1u) ? width : 8);
        if ((opcode & 0x07u) <= 0x03u) {
            const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                 rex_r, rex_x, rex_b, rex_w,
                                                 rex_present);
            const int direction = opcode & 0x02u;
            uint64_t destination = read_rm(cpu, &modrm, operation_width,
                                           rex_present);
            uint64_t source = operation_width == 8
                              ? get_reg8(cpu, modrm.reg, rex_present)
                              : get_gpr(cpu, modrm.reg, operation_width);
            if (direction) {
                source = destination;
                destination = operation_width == 8
                              ? get_reg8(cpu, modrm.reg, rex_present)
                              : get_gpr(cpu, modrm.reg, operation_width);
                const uint64_t result = wide_alu(cpu, operation, destination,
                                                 source, operation_width);
                if (operation_width == 8) {
                    set_reg8(cpu, modrm.reg, rex_present, (uint8_t)result);
                } else {
                    set_gpr(cpu, modrm.reg, operation_width, result);
                }
            } else {
                const uint64_t result = wide_alu(cpu, operation, destination,
                                                 source, operation_width);
                write_rm(cpu, &modrm, operation_width, rex_present, result);
            }
        } else {
            const uint64_t immediate = operation_width == 8 ? fetch8(cpu)
                                       : operation_width == 16 ? fetch16(cpu)
                                       : operation_width == 32 ? fetch32(cpu)
                                       : (uint64_t)fetch_s32(cpu);
            const uint64_t accumulator = operation_width == 8
                                         ? get_reg8(cpu, 0, rex_present)
                                         : get_gpr(cpu, X86_RAX, operation_width);
            const uint64_t result = wide_alu(cpu, operation, accumulator,
                                             immediate, operation_width);
            if (operation != 7) {
                if (operation_width == 8) {
                    set_reg8(cpu, 0, rex_present, (uint8_t)result);
                } else {
                    set_gpr(cpu, X86_RAX, operation_width, result);
                }
            }
        }
        done = 1;
    }

    if (!done) {
        switch (opcode) {
            case 0x50: case 0x51: case 0x52: case 0x53:
            case 0x54: case 0x55: case 0x56: case 0x57:
                wide_push(cpu, width, get_gpr(cpu, (opcode & 7) | (rex_b ? 8 : 0), width));
                break;
            case 0x58: case 0x59: case 0x5A: case 0x5B:
            case 0x5C: case 0x5D: case 0x5E: case 0x5F:
                set_gpr(cpu, (opcode & 7) | (rex_b ? 8 : 0), width,
                        wide_pop(cpu, width));
                break;
            case 0x68:
                wide_push(cpu, width, width == 16 ? fetch16(cpu) : (uint64_t)fetch32(cpu));
                break;
            case 0x6A:
                wide_push(cpu, width, (uint64_t)(int64_t)fetch_s8(cpu));
                break;
            case 0x63: {                 /* MOVSXD (только 64 бита) */
                if (width != 64) {
                    fault_wide(cpu, opcode, "MOVSXD доступен только в 64-битном режиме");
                    break;
                }
                const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                     rex_r, rex_x, rex_b, rex_w,
                                                     rex_present);
                const uint64_t value = read_rm(cpu, &modrm, 32, rex_present);
                set_gpr(cpu, modrm.reg, 64, (uint64_t)(int64_t)(int32_t)(uint32_t)value);
                break;
            }
            case 0x70: case 0x71: case 0x72: case 0x73:
            case 0x74: case 0x75: case 0x76: case 0x77:
            case 0x78: case 0x79: case 0x7A: case 0x7B:
            case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
                const int32_t displacement = fetch_s8(cpu);
                if (condition(cpu, opcode & 0x0F)) {
                    cpu->rip = (uint64_t)((int64_t)cpu->rip + displacement);
                }
                break;
            }
            case 0x80: case 0x81: case 0x83: {   /* Группа иммедиатов */
                const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                     rex_r, rex_x, rex_b, rex_w,
                                                     rex_present);
                const int operation_width = opcode == 0x80 ? 8 : width;
                const uint64_t immediate = opcode == 0x80 ? fetch8(cpu)
                                           : opcode == 0x81
                                             ? (operation_width == 16 ? fetch16(cpu)
                                                : operation_width == 64
                                                  ? (uint64_t)fetch_s32(cpu)
                                                  : fetch32(cpu))
                                             : (uint64_t)(int64_t)fetch_s8(cpu);
                const uint64_t value = read_rm(cpu, &modrm, operation_width,
                                               rex_present);
                const uint64_t result = wide_alu(cpu, modrm.reg, value,
                                                 immediate, operation_width);
                if (modrm.reg != 7) {
                    write_rm(cpu, &modrm, operation_width, rex_present, result);
                }
                break;
            }
            case 0x84: case 0x85: {
                const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                     rex_r, rex_x, rex_b, rex_w,
                                                     rex_present);
                const int operation_width = opcode == 0x84 ? 8 : width;
                const uint64_t value = read_rm(cpu, &modrm, operation_width,
                                               rex_present);
                const uint64_t source = operation_width == 8
                                        ? get_reg8(cpu, modrm.reg, rex_present)
                                        : get_gpr(cpu, modrm.reg, operation_width);
                wide_alu(cpu, 4, value, source, operation_width);
                break;
            }
            case 0x88: case 0x89: case 0x8A: case 0x8B: {
                const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                     rex_r, rex_x, rex_b, rex_w,
                                                     rex_present);
                const int operation_width = (opcode & 1u) ? width : 8;
                if (opcode == 0x88 || opcode == 0x89) {
                    const uint64_t value = operation_width == 8
                                           ? get_reg8(cpu, modrm.reg, rex_present)
                                           : get_gpr(cpu, modrm.reg, operation_width);
                    write_rm(cpu, &modrm, operation_width, rex_present, value);
                } else {
                    const uint64_t value = read_rm(cpu, &modrm, operation_width,
                                                   rex_present);
                    if (operation_width == 8) {
                        set_reg8(cpu, modrm.reg, rex_present, (uint8_t)value);
                    } else {
                        set_gpr(cpu, modrm.reg, operation_width, value);
                    }
                }
                break;
            }
            case 0x8D: {                 /* LEA */
                const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                     rex_r, rex_x, rex_b, rex_w,
                                                     rex_present);
                if (modrm.is_register) {
                    fault_wide(cpu, opcode, "LEA с регистровым операндом");
                    break;
                }
                const uint64_t offset = modrm.rip_relative
                                        ? (uint64_t)((int64_t)cpu->rip
                                                     + modrm.displacement)
                                        : modrm.offset;
                set_gpr(cpu, modrm.reg, width, offset);
                break;
            }
            case 0x8F: {                 /* POP r/m */
                const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                     rex_r, rex_x, rex_b, rex_w,
                                                     rex_present);
                write_rm(cpu, &modrm, width, rex_present, wide_pop(cpu, width));
                break;
            }
            case 0x90:                   /* NOP / XCHG RAX, r */
                if (rex_b) {
                    const uint64_t left = get_gpr(cpu, X86_RAX, width);
                    const uint64_t right = get_gpr(cpu, X86_R8, width);
                    set_gpr(cpu, X86_RAX, width, right);
                    set_gpr(cpu, X86_R8, width, left);
                }
                break;
            case 0xA8:
                wide_alu(cpu, 4, get_reg8(cpu, 0, rex_present), fetch8(cpu), 8);
                break;
            case 0xA9: {
                const uint64_t immediate = width == 16 ? fetch16(cpu)
                                           : width == 64 ? (uint64_t)fetch_s32(cpu)
                                           : fetch32(cpu);
                wide_alu(cpu, 4, get_gpr(cpu, X86_RAX, width), immediate, width);
                break;
            }
            case 0xB0: case 0xB1: case 0xB2: case 0xB3:
            case 0xB4: case 0xB5: case 0xB6: case 0xB7:
                set_reg8(cpu, (opcode & 7) | (rex_b ? 8 : 0), rex_present, fetch8(cpu));
                break;
            case 0xB8: case 0xB9: case 0xBA: case 0xBB:
            case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
                const uint64_t immediate = width == 8 ? (uint64_t)fetch8(cpu)
                                           : width == 16 ? fetch16(cpu)
                                           : width == 32 ? fetch32(cpu)
                                           : fetch64(cpu);
                set_gpr(cpu, (opcode & 7) | (rex_b ? 8 : 0), width, immediate);
                break;
            }
            case 0xC6: case 0xC7: {
                const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                     rex_r, rex_x, rex_b, rex_w,
                                                     rex_present);
                const int operation_width = opcode == 0xC6 ? 8 : width;
                const uint64_t immediate = operation_width == 8 ? fetch8(cpu)
                                           : operation_width == 16 ? fetch16(cpu)
                                           : operation_width == 32 ? fetch32(cpu)
                                           : (uint64_t)fetch_s32(cpu);
                write_rm(cpu, &modrm, operation_width, rex_present, immediate);
                break;
            }
            case 0xC3:                   /* RET */
                cpu->rip = wide_pop(cpu, width);
                break;
            case 0xC2: {                 /* RET imm16 */
                const uint16_t discard = fetch16(cpu);
                cpu->rip = wide_pop(cpu, width);
                set_gpr(cpu, X86_RSP, width == 64 ? 64 : 32,
                        (get_gpr(cpu, X86_RSP, width == 64 ? 64 : 32) + discard)
                        & width_mask(width == 64 ? 64 : 32));
                break;
            }
            case 0xE8: {                 /* CALL rel */
                const int64_t displacement = width == 16 ? fetch_s16(cpu) : fetch_s32(cpu);
                wide_push(cpu, width, cpu->rip);
                cpu->rip = (uint64_t)((int64_t)cpu->rip + displacement);
                break;
            }
            case 0xE9: {                 /* JMP rel */
                const int64_t displacement = width == 16 ? fetch_s16(cpu) : fetch_s32(cpu);
                cpu->rip = (uint64_t)((int64_t)cpu->rip + displacement);
                break;
            }
            case 0xEB: {                 /* JMP rel8 */
                const int64_t displacement = fetch_s8(cpu);
                cpu->rip = (uint64_t)((int64_t)cpu->rip + displacement);
                break;
            }
            case 0xEA: {                 /* JMP far ptr16:16 / ptr16:32 */
                const uint64_t offset = width == 16 ? fetch16(cpu) : fetch32(cpu);
                const uint16_t selector = fetch16(cpu);
                load_descriptor(cpu, selector, offset, opcode);
                break;
            }
            case 0xFF: {
                const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                     rex_r, rex_x, rex_b, rex_w,
                                                     rex_present);
                const uint64_t value = read_rm(cpu, &modrm, width, rex_present);
                switch (modrm.reg) {
                    case 0: { const uint64_t saved = flag(cpu, X86_CF);
                              const uint64_t result = wide_alu(cpu, 0, value, 1, width);
                              write_rm(cpu, &modrm, width, rex_present, result);
                              set_flag(cpu, X86_CF, saved != 0); break; }
                    case 1: { const uint64_t saved = flag(cpu, X86_CF);
                              const uint64_t result = wide_alu(cpu, 5, value, 1, width);
                              write_rm(cpu, &modrm, width, rex_present, result);
                              set_flag(cpu, X86_CF, saved != 0); break; }
                    case 2: wide_push(cpu, width, cpu->rip); cpu->rip = value; break;
                    case 4: cpu->rip = value; break;
                    case 6: wide_push(cpu, width, value); break;
                    default:
                        fault_wide(cpu, opcode, "Неподдерживаемый подкод группы FFh");
                        break;
                }
                break;
            }
            case 0xF4: cpu->halted = 1; break;
            case 0xF8: set_flag(cpu, X86_CF, 0); break;
            case 0xF9: set_flag(cpu, X86_CF, 1); break;
            case 0xFA: set_flag(cpu, X86_IF, 0); break;
            case 0xFB: set_flag(cpu, X86_IF, 1); break;
            case 0xCD: {
                const uint8_t vector = fetch8(cpu);
                x86_wide_sync_to_real(cpu);
                if (cpu->interrupt_hook != NULL &&
                    cpu->interrupt_hook(cpu->context, vector)) {
                    x86_wide_sync_from_real(cpu);
                } else {
                    const uint64_t entry = (uint64_t)vector * 4u;
                    const uint64_t target = read_mem(cpu, entry, 16);
                    const uint64_t segment_value = read_mem(cpu, entry + 2u, 16);
                    wide_push(cpu, width, cpu->flags);
                    wide_push(cpu, width, cpu->rip);
                    cpu->seg_base[X86_CS] = segment_value * 16u;
                    cpu->rip = target;
                }
                break;
            }
            case 0x0F: {
                const uint8_t subopcode = fetch8(cpu);
                wide_0f(cpu, subopcode, width, address_width, segment,
                        rex_r, rex_x, rex_b, rex_w, rex_present);
                break;
            }
            default:
                fault_wide(cpu, opcode,
                           "Неподдерживаемая инструкция в 32/64-битном режиме");
                break;
        }
    }

    x86_wide_sync_to_real(cpu);
    if (cpu->waiting) {
        /* Как и в 16-битном ядре: INT повторится, когда придёт клавиша. */
        cpu->rip = start_rip;
    }
    if (cpu->mode == X86_MODE_16) {
        cpu->ip = (uint16_t)cpu->rip;
    }
    return cpu->halted || cpu->waiting || cpu->fault ? 0 : 1;
}
