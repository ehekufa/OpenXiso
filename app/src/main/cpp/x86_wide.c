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
        cpu->flags &= ~(uint32_t)mask;
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
        address &= ~0x100000ULL;
        address &= 0xFFFFFULL;
    }
    if (cpu->mode == X86_MODE_32) {
        address &= 0xFFFFFFFFULL;
    }
    return address;
}

static int translate_access(X86Cpu* cpu, uint64_t linear, int write, int fetch,
                            uint64_t* physical) {
    int pf = -1;
    *physical = x86_translate(cpu, linear, write, fetch, &pf);
    if (pf < 0) return 1;
    cpu->pf_pending = 1;
    cpu->pf_error = (uint32_t)pf;
    cpu->pf_linear = linear;
    cpu->instr_abort = 1;
    return 0;
}

static uint64_t read_mem_access(X86Cpu* cpu, uint64_t linear, int width, int fetch) {
    if (cpu->pf_pending) return 0;
    uint64_t physical;
    if (!translate_access(cpu, linear, 0, fetch, &physical)) return 0;
    int bytes = width / 8;
    if ((linear & 0xfffu) + (unsigned)bytes <= 4096)
        return x86_read_physical(cpu, physical, width);
    uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) {
        if (i && !translate_access(cpu, linear + (uint64_t)i, 0, fetch, &physical)) return 0;
        value |= x86_read_physical(cpu, physical, 8) << (8 * i);
    }
    return value;
}

static uint64_t read_mem(X86Cpu* cpu, uint64_t linear, int width) {
    return read_mem_access(cpu, linear, width, 0);
}

static void write_mem(X86Cpu* cpu, uint64_t linear, int width, uint64_t value) {
    if (cpu->pf_pending) return;
    uint64_t physical[8];
    int bytes = width / 8;
    if (!translate_access(cpu, linear, 1, 0, &physical[0])) return;
    if ((linear & 0xfffu) + (unsigned)bytes <= 4096) {
        x86_write_physical(cpu, physical[0], width, value);
        return;
    }
    /* Validate every page before making any visible writes. */
    for (int i = 1; i < bytes; ++i)
        if (!translate_access(cpu, linear + (uint64_t)i, 1, 0, &physical[i])) return;
    for (int i = 0; i < bytes; ++i)
        x86_write_physical(cpu, physical[i], 8, (value >> (8 * i)) & 0xff);
}

static uint8_t fetch8(X86Cpu* cpu) {
    const uint64_t address = linearize(cpu, cpu->rip, X86_CS);
    const uint8_t value = (uint8_t)read_mem_access(cpu, address, 8, 1);
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
        /* 16-bit effective addresses wrap, including direct disp16 >= 8000h. */
        result.offset = (uint16_t)((uint64_t)base + (uint64_t)displacement);
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
    const int pointer_width = cpu->mode == X86_MODE_64 ? 64 :
                              cpu->mode == X86_MODE_16 ? 16 : 32;
    const uint64_t stack = (get_gpr(cpu, X86_RSP, pointer_width)
                            - (uint64_t)bytes) & width_mask(pointer_width);
    set_gpr(cpu, X86_RSP, pointer_width, stack);
    write_mem(cpu, linearize(cpu, stack, X86_SS), width, value);
}

static uint64_t wide_pop(X86Cpu* cpu, int width) {
    const int bytes = width / 8;
    const int pointer_width = cpu->mode == X86_MODE_64 ? 64 :
                              cpu->mode == X86_MODE_16 ? 16 : 32;
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
        cpu->efer &= ~X86_EFER_LMA;
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
    if ((cpu->efer & X86_EFER_LME) && (cpu->cr0 & X86_CR0_PG) && (cpu->cr0 & X86_CR0_PE)) {
        cpu->efer |= X86_EFER_LMA;
    } else if (!(cpu->cr0 & X86_CR0_PG)) {
        cpu->efer &= ~X86_EFER_LMA;
    }
}

/* The 16-bit interpreter and wide interpreter have separate register views. */
static void wide_exception(X86Cpu* cpu, uint8_t vector, uint32_t error) {
    if (!(cpu->cr0 & X86_CR0_PE)) {
        cpu->ip = (uint16_t)cpu->rip;
        x86_wide_sync_to_real(cpu);
        x86_deliver_exception(cpu, vector, error);
        x86_wide_sync_from_real(cpu);
        cpu->rip = cpu->ip;
    } else {
        x86_deliver_exception(cpu, vector, error);
    }
}

static void load_descriptor(X86Cpu* cpu, uint16_t selector, uint64_t offset,
                            uint8_t opcode) {
    if (!(cpu->cr0 & X86_CR0_PE)) {
        x86_load_segment(cpu, X86_CS, selector);
        cpu->rip = (uint16_t)offset;
        cpu->ip = (uint16_t)offset;
        return;
    }
    if (x86_load_segment(cpu, X86_CS, selector) != 0) {
        /* #GP */
        cpu->rip = cpu->instr_start_rip;
        wide_exception(cpu, 13, (uint32_t)selector);
        return;
    }
    uint8_t access = cpu->seg_attr[X86_CS];
    uint8_t flags = cpu->seg_flags[X86_CS];
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
    if (!(access & 0x80)) {
        cpu->rip = cpu->instr_start_rip;
        wide_exception(cpu, 13, (uint32_t)selector);
        return;
    }
    cpu->rip = offset;
    cpu->ip = (uint16_t)offset;
    if (long_mode) {
        cpu->mode = X86_MODE_64;
        cpu->efer |= X86_EFER_LMA;
    } else if (default_32) {
        cpu->mode = X86_MODE_32;
    } else {
        cpu->mode = X86_MODE_16;
    }
    cpu->cpl = (selector >> 0) & 3;
}

static void wide_0f(X86Cpu* cpu, uint8_t subopcode, int width, int address_width,
                    int segment, int rex_r, int rex_x, int rex_b, int rex_w,
                    int rex_present) {
    if (subopcode >= 0x80 && subopcode <= 0x8F) {
        const int32_t displacement = width == 16 ? fetch_s16(cpu) : fetch_s32(cpu);
        if (condition(cpu, subopcode & 0x0F)) {
            cpu->rip = (uint64_t)((int64_t)cpu->rip + displacement);
            if (width == 16) cpu->rip &= 0xffffu;
        }
        return;
    }

    switch (subopcode) {
        case 0x00: { /* SLDT, STR, LLDT, LTR */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                 rex_r, rex_x, rex_b, rex_w, rex_present);
            if (!(cpu->cr0 & X86_CR0_PE) || modrm.reg > 3) {
                cpu->rip = cpu->instr_start_rip; wide_exception(cpu, 6, 0); return;
            }
            if (modrm.reg <= 1) {
                uint16_t selector = modrm.reg ? cpu->tr_sel : cpu->ldtr_sel;
                write_rm(cpu, &modrm, modrm.is_register ? width : 16, rex_present, selector);
                return;
            }
            uint16_t selector = (uint16_t)read_rm(cpu, &modrm, 16, rex_present);
            if (cpu->pf_pending) return;
            if (cpu->cpl != 0) {
                cpu->rip = cpu->instr_start_rip; wide_exception(cpu, 13, 0); return;
            }
            if (modrm.reg == 2 && (selector & 0xfffcu) == 0) {
                cpu->ldtr_sel = 0; cpu->ldtr_base = 0; cpu->ldtr_limit = 0;
                return;
            }
            const uint32_t index = selector & 0xfff8u;
            const unsigned descriptor_size = (cpu->efer & X86_EFER_LMA) ? 16 : 8;
            if ((selector & 4u) || !index || index + descriptor_size - 1 > cpu->gdtr_limit) {
                cpu->rip = cpu->instr_start_rip; wide_exception(cpu, 13, selector & 0xfffcu); return;
            }
            const uint64_t entry = cpu->gdtr_base + index;
            const uint64_t d = read_mem(cpu, entry, 64);
            uint64_t high = descriptor_size == 16 ? read_mem(cpu, entry + 8, 64) : 0;
            if (cpu->pf_pending) return;
            const uint8_t access = (uint8_t)(d >> 40);
            const uint8_t type = access & 0x0fu;
            if ((access & 0x10u) || (modrm.reg == 2 ? type != 2 : (type != 1 && type != 9)) ||
                (descriptor_size == 16 && ((high >> 32) != 0 || (modrm.reg == 3 && type != 9)))) {
                cpu->rip = cpu->instr_start_rip; wide_exception(cpu, 13, selector & 0xfffcu); return;
            }
            if (!(access & 0x80u)) {
                cpu->rip = cpu->instr_start_rip; wide_exception(cpu, 11, selector & 0xfffcu); return;
            }
            uint64_t base = ((d >> 16) & 0xffffffu) | ((d >> 32) & 0xff000000u);
            if (descriptor_size == 16) base |= (high & 0xffffffffu) << 32;
            uint32_t limit = (uint32_t)((d & 0xffffu) | ((d >> 32) & 0xf0000u));
            if (d & (UINT64_C(1) << 55)) limit = (limit << 12) | 0xfffu;
            if (modrm.reg == 2) {
                cpu->ldtr_sel = selector; cpu->ldtr_base = base; cpu->ldtr_limit = limit;
            } else {
                write_mem(cpu, entry + 5, 8, access | 2u); /* TSS becomes busy */
                if (cpu->pf_pending) return;
                cpu->tr_sel = selector; cpu->tr_base = base; cpu->tr_limit = limit;
            }
            return;
        }
        case 0x01: {                     /* LGDT / LIDT / SGDT / SIDT */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                 rex_r, rex_x, rex_b, rex_w,
                                                 rex_present);
            const uint64_t address = modrm_address(cpu, &modrm);
            if (modrm.reg == 2 || modrm.reg == 3) {
                const uint16_t limit = (uint16_t)read_mem(cpu, address, 16);
                uint64_t base = read_mem(cpu, address + 2u,
                                         width == 64 ? 64 : 32);
                if (width == 16) base &= 0x00ffffffu;
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
        case 0x30: {
            uint32_t msr = (uint32_t)get_gpr(cpu, X86_RCX, 32);
            uint64_t v = (get_gpr(cpu, X86_RDX, 32) << 32) | get_gpr(cpu, X86_RAX, 32);
            x86_write_msr(cpu, msr, v);
            return;
        }
        case 0x32: {
            uint32_t msr = (uint32_t)get_gpr(cpu, X86_RCX, 32);
            uint64_t v = x86_read_msr(cpu, msr);
            set_gpr(cpu, X86_RAX, 32, (uint32_t)v);
            set_gpr(cpu, X86_RDX, 32, (uint32_t)(v >> 32));
            return;
        }
        case 0x31: { /* RDTSC */
            set_gpr(cpu, X86_RAX, 32, (uint32_t)cpu->tsc);
            set_gpr(cpu, X86_RDX, 32, (uint32_t)(cpu->tsc >> 32));
            return;
        }
        case 0xA2: {
            const uint32_t leaf = (uint32_t)get_gpr(cpu, X86_RAX, 32);
            const uint32_t sub = (uint32_t)get_gpr(cpu, X86_RCX, 32);
            if (leaf == 0) {
                set_gpr(cpu, X86_RAX, 32, 1u);
                set_gpr(cpu, X86_RBX, 32, 0x756E6547u);
                set_gpr(cpu, X86_RCX, 32, 0x6C65746Eu);
                set_gpr(cpu, X86_RDX, 32, 0x49656E69u);
            } else if (leaf == 1u) {
                set_gpr(cpu, X86_RAX, 32, 0x000906E9u);
                set_gpr(cpu, X86_RBX, 32, 0x00080000u);
                /* Do not advertise instruction sets whose implementation is incomplete. */
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, (1u<<4) | (1u<<5) | (1u<<15));
            } else if (leaf == 4u) {
                set_gpr(cpu, X86_RAX, 32, 0u);
                set_gpr(cpu, X86_RBX, 32, 0u);
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, 0u);
            } else if (leaf == 6u) {
                set_gpr(cpu, X86_RAX, 32, 0u);
                set_gpr(cpu, X86_RBX, 32, 0u);
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, 0u);
            } else if (leaf == 7u && sub == 0) {
                set_gpr(cpu, X86_RAX, 32, 0u);
                set_gpr(cpu, X86_RBX, 32, 0u); /* No BMI/AVX support advertised. */
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, 0u);
            } else if (leaf == 0x80000000u) {
                set_gpr(cpu, X86_RAX, 32, 0x80000008u);
                set_gpr(cpu, X86_RBX, 32, 0u);
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, 0u);
            } else if (leaf == 0x80000001u) {
                set_gpr(cpu, X86_RAX, 32, 0x000906E9u);
                set_gpr(cpu, X86_RBX, 32, 0u);
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, cpu->max_mode == X86_MODE_64 ? (1u<<29) : 0u);
            } else if (leaf == 0x80000006u) {
                set_gpr(cpu, X86_RAX, 32, 0u);
                set_gpr(cpu, X86_RBX, 32, 0u);
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, 0u);
            } else if (leaf == 0x80000007u) {
                set_gpr(cpu, X86_RAX, 32, 0u);
                set_gpr(cpu, X86_RBX, 32, 0u);
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, 0u);
            } else if (leaf == 0x80000008u) {
                set_gpr(cpu, X86_RAX, 32, 0x00003028u); /* phys 40, virt 48 */
                set_gpr(cpu, X86_RBX, 32, 0u);
                set_gpr(cpu, X86_RCX, 32, 0u);
                set_gpr(cpu, X86_RDX, 32, 0u);
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
        case 0x05: { /* SYSCALL */
            if (!(cpu->efer & X86_EFER_SCE)) { cpu->rip = cpu->instr_start_rip; wide_exception(cpu, 6, 0); return; }
            uint64_t star = cpu->msr_star;
            uint16_t cs_sel = (uint16_t)(star >> 32);
            uint16_t ss_sel = (uint16_t)((star >> 32) + 16);
            set_gpr(cpu, X86_RCX, 64, cpu->rip);
            set_gpr(cpu, X86_R11, 64, cpu->flags);
            cpu->flags &= ~(uint32_t)cpu->msr_fmask;
            cpu->flags &= ~(uint32_t)(X86_IF|X86_TF|X86_VM);
            if (x86_load_segment(cpu, X86_CS, cs_sel) == 0) {
                cpu->seg_sel[X86_CS] = cs_sel;
            }
            if (x86_load_segment(cpu, X86_SS, ss_sel) == 0) {
                cpu->seg_sel[X86_SS] = ss_sel;
            }
            cpu->rip = cpu->msr_lstar;
            cpu->cpl = 0;
            return;
        }
        case 0x07: { /* SYSRET */
            uint64_t star = cpu->msr_star;
            uint16_t cs_sel = (uint16_t)((star >> 32) + 16);
            uint16_t ss_sel = (uint16_t)(star >> 32);
            cpu->flags = (uint16_t)(cpu->gpr[X86_R11] | ((uint32_t)cpu->msr_fmask << 0));
            /* Actually SYSRET restores flags from R11 masked by FMASK inverted? Simplified: */
            cpu->flags = (uint32_t)cpu->gpr[X86_R11];
            if (x86_load_segment(cpu, X86_CS, cs_sel) == 0) cpu->seg_sel[X86_CS] = cs_sel;
            if (x86_load_segment(cpu, X86_SS, ss_sel) == 0) cpu->seg_sel[X86_SS] = ss_sel;
            cpu->rip = cpu->gpr[X86_RCX];
            cpu->cpl = 3;
            return;
        }
        case 0x0B: /* UD2 */
            cpu->rip = cpu->instr_start_rip;
            wide_exception(cpu, 6, 0);
            return;
        case 0x0D: { /* PREFETCH / NOP Ev */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            (void)modrm;
            return;
        }
        case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: { /* PREFETCHNTA etc / NOP */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            (void)modrm;
            return;
        }
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: { /* CMOVcc */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            if (condition(cpu, subopcode & 0x0F)) {
                uint64_t v = read_rm(cpu, &modrm, width, rex_present);
                set_gpr(cpu, modrm.reg, width, v);
            }
            return;
        }
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: { /* SETcc */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            write_rm(cpu, &modrm, 8, rex_present, condition(cpu, subopcode & 0x0F) ? 1 : 0);
            return;
        }
        case 0xA3: case 0xAB: case 0xB3: case 0xBB: { /* BT/BTS/BTR/BTC r/m, r */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            uint64_t bit = get_gpr(cpu, modrm.reg, width);
            uint64_t val = read_rm(cpu, &modrm, width, rex_present);
            int bitpos = (int)(bit & (width - 1));
            int old = (val >> bitpos) & 1;
            set_flag(cpu, X86_CF, old);
            if (subopcode == 0xA3) { /* BT */ }
            else if (subopcode == 0xAB) { val |= (1ULL << bitpos); write_rm(cpu, &modrm, width, rex_present, val); }
            else if (subopcode == 0xB3) { val &= ~(1ULL << bitpos); write_rm(cpu, &modrm, width, rex_present, val); }
            else { val ^= (1ULL << bitpos); write_rm(cpu, &modrm, width, rex_present, val); }
            return;
        }
        case 0xA4: case 0xA5: case 0xAC: case 0xAD: { /* SHLD/SHRD r/m, r, imm8/CL */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            uint8_t count;
            if (subopcode == 0xA4 || subopcode == 0xAC) count = fetch8(cpu);
            else count = (uint8_t)get_reg8(cpu, 1, rex_present);
            count &= (width == 64 ? 63 : 31);
            uint64_t val = read_rm(cpu, &modrm, width, rex_present);
            uint64_t src = get_gpr(cpu, modrm.reg, width);
            uint64_t res;
            if (subopcode == 0xA4 || subopcode == 0xA5) { /* SHLD */
                if (count == 0) return;
                res = (val << count) | (src >> (width - count));
                set_flag(cpu, X86_CF, (res >> (width-1)) & 1 ? ((val >> (width-count)) & 1) : 0);
                /* Simplified CF */
                set_flag(cpu, X86_CF, (val >> (width - count)) & 1);
            } else {
                if (count == 0) return;
                res = (val >> count) | (src << (width - count));
                set_flag(cpu, X86_CF, (val >> (count-1)) & 1);
            }
            write_rm(cpu, &modrm, width, rex_present, res & width_mask(width));
            return;
        }
        case 0xA0: case 0xA1: case 0xA8: case 0xA9: {
            int seg = subopcode < 0xA8 ? X86_FS : X86_GS;
            int stack_width = cpu->mode == X86_MODE_64 && width != 16 ? 64 : width;
            if ((subopcode & 1) == 0) {
                wide_push(cpu, stack_width, cpu->sregs[seg]);
            } else {
                uint16_t selector = (uint16_t)wide_pop(cpu, stack_width);
                if (x86_load_segment(cpu, seg, selector) != 0) {
                    cpu->rip = cpu->instr_start_rip;
                    wide_exception(cpu, 13, selector);
                }
            }
            return;
        }
        case 0xAA: case 0xB2: { /* RSM / LSS */
            if (subopcode == 0xAA) { return; }
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            if (modrm.is_register) { cpu->rip=cpu->instr_start_rip; wide_exception(cpu,13,0); return; }
            uint64_t addr = modrm_address(cpu, &modrm);
            uint64_t off = read_mem(cpu, addr, width == 16 ? 16 : 32);
            uint16_t sel = (uint16_t)read_mem(cpu, addr + (width==16?2:4), 16);
            set_gpr(cpu, modrm.reg, width, off);
            x86_load_segment(cpu, X86_SS, sel);
            return;
        }
        case 0xB4: case 0xB5: { /* LFS/LGS */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            if (modrm.is_register) { cpu->rip=cpu->instr_start_rip; wide_exception(cpu,13,0); return; }
            uint64_t addr = modrm_address(cpu, &modrm);
            uint64_t off = read_mem(cpu, addr, width == 16 ? 16 : 32);
            uint16_t sel = (uint16_t)read_mem(cpu, addr + (width==16?2:4), 16);
            set_gpr(cpu, modrm.reg, width, off);
            x86_load_segment(cpu, subopcode==0xB4?X86_FS:X86_GS, sel);
            return;
        }
        case 0xBA: case 0xC8: case 0xC9: { /* BT imm, BSWAP */
            if (subopcode == 0xBA) {
                const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
                uint8_t bit = fetch8(cpu);
                uint64_t val = read_rm(cpu, &modrm, width, rex_present);
                int bitpos = bit & (width-1);
                set_flag(cpu, X86_CF, (val>>bitpos)&1);
                return;
            } else {
                /* BSWAP r */
                int reg = (subopcode & 7) | (rex_b ? 8 : 0);
                uint64_t v = get_gpr(cpu, reg, width);
                uint64_t r=0;
                for (int i=0;i<width/8;i++) r |= ((v >> (8*i)) & 0xFF) << (width-8-8*i);
                set_gpr(cpu, reg, width, r);
                return;
            }
        }
        case 0xBC: case 0xBD: { /* BSF/BSR */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            uint64_t v = read_rm(cpu, &modrm, width, rex_present);
            if (v==0) { set_flag(cpu, X86_ZF, 1); return; }
            set_flag(cpu, X86_ZF, 0);
            int pos=0;
            if (subopcode==0xBC) { while(((v>>pos)&1)==0) pos++; }
            else { pos=width-1; while(((v>>pos)&1)==0) pos--; }
            set_gpr(cpu, modrm.reg, width, pos);
            return;
        }
        case 0xBE: case 0xBF: { /* MOVSX r, r/m8/16 */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            int srcw = subopcode==0xBE?8:16;
            uint64_t v = read_rm(cpu, &modrm, srcw, rex_present);
            int64_t sv;
            if (srcw==8) sv=(int8_t)v; else sv=(int16_t)v;
            set_gpr(cpu, modrm.reg, width, (uint64_t)sv);
            return;
        }
        case 0xC0: case 0xC1: { /* XADD */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            uint64_t a = read_rm(cpu, &modrm, width, rex_present);
            uint64_t b = get_gpr(cpu, modrm.reg, width);
            uint64_t res = wide_alu(cpu, 0, a, b, width);
            write_rm(cpu, &modrm, width, rex_present, res);
            set_gpr(cpu, modrm.reg, width, a);
            return;
        }
        case 0xC7: { /* CMPXCHG8B/16B, VMPTRLD etc */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            if (modrm.reg==1) {
                /* CMPXCHG8B/16B */
                uint64_t addr = modrm_address(cpu, &modrm);
                uint64_t lo = read_mem(cpu, addr, 64);
                uint64_t hi = read_mem(cpu, addr+8, 64);
                uint64_t eax = get_gpr(cpu, X86_RAX, 32);
                uint64_t edx = get_gpr(cpu, X86_RDX, 32);
                uint64_t ecx = get_gpr(cpu, X86_RCX, 32);
                uint64_t ebx = get_gpr(cpu, X86_RBX, 32);
                if (lo==eax && hi==edx) {
                    set_flag(cpu, X86_ZF, 1);
                    write_mem(cpu, addr, 64, ebx);
                    write_mem(cpu, addr+8, 64, ecx);
                } else {
                    set_flag(cpu, X86_ZF, 0);
                    set_gpr(cpu, X86_RAX, 32, (uint32_t)lo);
                    set_gpr(cpu, X86_RDX, 32, (uint32_t)(lo>>32));
                    set_gpr(cpu, X86_RCX, 32, (uint32_t)hi);
                    set_gpr(cpu, X86_RBX, 32, (uint32_t)(hi>>32));
                }
                return;
            }
            return;
        }
        case 0xAE: { /* FXSAVE/FXRSTOR/LDMXCSR/STMXCSR/XSAVE etc */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            uint64_t addr = modrm_address(cpu, &modrm);
            if (modrm.reg==0) { /* FXSAVE */
                write_mem(cpu, addr, 16, cpu->x87_cw);
                write_mem(cpu, addr+2, 16, cpu->x87_sw);
                write_mem(cpu, addr+4, 16, 0xFFFF);
                write_mem(cpu, addr+6, 16, 0xFFFF);
                for(int i=0;i<8;i++) for(int j=0;j<10;j++) write_mem(cpu, addr+8+i*16+j, 8, cpu->x87_stack[i][j]);
                for(int i=0;i<16;i++) for(int j=0;j<16;j++) write_mem(cpu, addr+8+128+i*16+j, 8, cpu->xmm[i][j]);
                write_mem(cpu, addr+8+128+256, 32, 0x00000002);
                return;
            } else if (modrm.reg==1) { /* FXRSTOR */
                cpu->x87_cw = (uint16_t)read_mem(cpu, addr, 16);
                cpu->x87_sw = (uint16_t)read_mem(cpu, addr+2, 16);
                for(int i=0;i<8;i++) for(int j=0;j<10;j++) cpu->x87_stack[i][j]=(uint8_t)read_mem(cpu, addr+8+i*16+j,8);
                for(int i=0;i<16;i++) for(int j=0;j<16;j++) cpu->xmm[i][j]=(uint8_t)read_mem(cpu, addr+8+128+i*16+j,8);
                return;
            } else if (modrm.reg==2) { /* LDMXCSR */
                return;
            } else if (modrm.reg==3) { /* STMXCSR */
                write_mem(cpu, addr, 32, 0x1F80);
                return;
            } else if (modrm.reg==4) { /* XSAVE */
                write_mem(cpu, addr, 32, 0x00000002);
                return;
            } else if (modrm.reg==5) { /* XRSTOR / LFENCE */
                return;
            } else if (modrm.reg==6) { /* MFENCE / CLFLUSH */
                return;
            } else if (modrm.reg==7) { /* SFENCE / CLFLUSH */
                return;
            }
            return;
        }
        case 0xC3: { /* MOVNTI */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            uint64_t v = get_gpr(cpu, modrm.reg, width);
            write_rm(cpu, &modrm, width, rex_present, v);
            return;
        }
        case 0xC4: case 0xC5: { /* PINSRW / PEXTRW etc - SSE handled elsewhere, treat as NOP for integer */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            (void)modrm; fetch8(cpu);
            return;
        }
        case 0xC6: { /* SHUFPS etc */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            (void)modrm; fetch8(cpu);
            return;
        }
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE: case 0xEF:
        case 0xF0: case 0xF1: case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF: {
            /* SSE/SSE2: MOVAPS/MOVUPS/MOVDQA/PXOR etc. Simplified: handle common moves */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            int xmm_reg = modrm.reg & 15;
            /* For MOVAPS/MOVUPS (28/29), MOVDQA (6F/7F), MOVDQU (F3 6F/7F), PXOR (EF), etc. */
            /* We approximate: if subopcode is move load, load 16 bytes into xmm; if store, store xmm to mem */
            if (subopcode==0x28 || subopcode==0x10 || subopcode==0x6F) {
                if (modrm.is_register) {
                    int src = modrm.rm & 15;
                    for(int i=0;i<16;i++) cpu->xmm[xmm_reg][i]=cpu->xmm[src][i];
                } else {
                    uint64_t addr = modrm_address(cpu, &modrm);
                    for(int i=0;i<16;i++) cpu->xmm[xmm_reg][i]=(uint8_t)read_mem(cpu, addr+i, 8);
                }
            } else if (subopcode==0x29 || subopcode==0x11 || subopcode==0x7F) {
                if (modrm.is_register) {
                    int dst = modrm.rm & 15;
                    for(int i=0;i<16;i++) cpu->xmm[dst][i]=cpu->xmm[xmm_reg][i];
                } else {
                    uint64_t addr = modrm_address(cpu, &modrm);
                    for(int i=0;i<16;i++) write_mem(cpu, addr+i, 8, cpu->xmm[xmm_reg][i]);
                }
            } else if (subopcode==0xEF) { /* PXOR */
                uint8_t src[16];
                if (modrm.is_register) { int s=modrm.rm&15; for(int i=0;i<16;i++) src[i]=cpu->xmm[s][i]; }
                else { uint64_t addr=modrm_address(cpu,&modrm); for(int i=0;i<16;i++) src[i]=(uint8_t)read_mem(cpu,addr+i,8); }
                for(int i=0;i<16;i++) cpu->xmm[xmm_reg][i]^=src[i];
            } else if (subopcode==0xDB || subopcode==0xEB || subopcode==0xF8 || subopcode==0xFC) {
                /* PAND/POR/PADD etc: do integer ops on 128-bit as 2x64 */
                uint8_t src[16];
                if (modrm.is_register) { int s=modrm.rm&15; for(int i=0;i<16;i++) src[i]=cpu->xmm[s][i]; }
                else { uint64_t addr=modrm_address(cpu,&modrm); for(int i=0;i<16;i++) src[i]=(uint8_t)read_mem(cpu,addr+i,8); }
                for(int i=0;i<16;i++) {
                    if(subopcode==0xDB) cpu->xmm[xmm_reg][i]&=src[i];
                    else if(subopcode==0xEB) cpu->xmm[xmm_reg][i]|=src[i];
                    else if(subopcode==0xFC) cpu->xmm[xmm_reg][i]+=src[i];
                    else cpu->xmm[xmm_reg][i]^=src[i];
                }
            } else if (subopcode==0x2A || subopcode==0x2C || subopcode==0x2D) {
                /* CVTPI2PS etc: NOP */
            } else {
                /* Unknown SSE: treat as NOP to allow boot, but consume modrm already */
            }
            return;
        }
        case 0x38: case 0x3A: {
            /* SSSE3/SSE4: POPCNT is F3 0F B8, handled in main decoder via prefix? Here subopcode is 38/3A escape */
            uint8_t sub2 = fetch8(cpu);
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            (void)sub2; (void)modrm;
            if (sub2==0xF0 || sub2==0xF1) { /* MOVBE / CRC32 */
                uint64_t v = read_rm(cpu, &modrm, width, rex_present);
                /* byte swap for MOVBE load */
                uint64_t r=0; for(int i=0;i<width/8;i++) r|=((v>>(8*i))&0xFF)<<(width-8-8*i);
                set_gpr(cpu, modrm.reg, width, r);
            }
            return;
        }
        case 0xB8: { /* POPCNT / JMPE - with F3 prefix it's POPCNT, without it's JMPE (IA64) */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            uint64_t v = read_rm(cpu, &modrm, width, rex_present);
            int cnt=0; while(v){cnt+=v&1; v>>=1;}
            set_gpr(cpu, modrm.reg, width, cnt);
            set_flag(cpu, X86_ZF, cnt==0);
            return;
        }
        case 0xC2: { /* CMPPS etc */
            const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
            (void)modrm; fetch8(cpu);
            return;
        }
        default:
            /* For unknown 0F, deliver #UD instead of halting whole VM - guest may probe */
            cpu->rip = cpu->instr_start_rip;
            wide_exception(cpu, 6, 0);
            return;
    }
}

/* ---------------- Одна инструкция широкого ядра ---------------- */

int x86_wide_step(X86Cpu* cpu) {
    if (cpu->fault) return 0;
    if (cpu->waiting) return 0;
    if (cpu->halted) {
        if (x86_check_and_deliver_irq(cpu)) { cpu->instructions+=1; return 1; }
        return 0;
    }
    if (cpu->sti_shadow > 0) cpu->sti_shadow--;
    else if (x86_check_and_deliver_irq(cpu)) { cpu->instructions+=1; return 1; }
    cpu->tsc += 3;
    cpu->pf_pending = 0;
    cpu->instr_abort = 0;
    wide_initialize(cpu);
    if (cpu->mode == X86_MODE_16) {
        cpu->rip = (uint64_t)cpu->ip;
        if (!(cpu->cr0 & X86_CR0_PE)) {
            for (int index = 0; index < 6; ++index) {
                /* Do not discard hidden descriptor caches when PE is cleared.
                 * Resync only if an external real-mode hook changed a selector. */
                if (cpu->seg_sel[index] != cpu->sregs[index])
                    x86_load_segment(cpu, index, cpu->sregs[index]);
            }
        }
    }
    cpu->instructions += 1u;
    const uint64_t start_rip = cpu->rip;
    cpu->instr_start_rip = start_rip;

    int operand_override = 0;
    int address_override = 0;
    int segment = X86_DS;
    int rex_r = 0, rex_x = 0, rex_b = 0, rex_w = 0, rex_present = 0;
    int repetition = 0;
    uint8_t opcode = 0;

    for (;;) {
        opcode = fetch8(cpu);
        if (opcode == 0xF0) continue;
        if (opcode == 0xF2) { repetition = -1; continue; }
        if (opcode == 0xF3) { repetition = 1; continue; }
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
                if (operation != 7) {
                    if (operation_width == 8) set_reg8(cpu, modrm.reg, rex_present, (uint8_t)result);
                    else set_gpr(cpu, modrm.reg, operation_width, result);
                }
            } else {
                const uint64_t result = wide_alu(cpu, operation, destination,
                                                 source, operation_width);
                if (operation != 7) write_rm(cpu, &modrm, operation_width, rex_present, result);
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
            case 0x40: case 0x41: case 0x42: case 0x43:
            case 0x44: case 0x45: case 0x46: case 0x47:
            case 0x48: case 0x49: case 0x4A: case 0x4B:
            case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
                /* In long mode these bytes are consumed as REX prefixes. */
                int reg = opcode & 7;
                int carry = flag(cpu, X86_CF);
                uint64_t result = wide_alu(cpu, opcode < 0x48 ? 0 : 5,
                                           get_gpr(cpu, reg, width), 1, width);
                set_gpr(cpu, reg, width, result);
                set_flag(cpu, X86_CF, carry);
                break;
            }
            case 0x50: case 0x51: case 0x52: case 0x53:
            case 0x54: case 0x55: case 0x56: case 0x57:
                wide_push(cpu, width, get_gpr(cpu, (opcode & 7) | (rex_b ? 8 : 0), width));
                break;
            case 0x58: case 0x59: case 0x5A: case 0x5B:
            case 0x5C: case 0x5D: case 0x5E: case 0x5F:
                set_gpr(cpu, (opcode & 7) | (rex_b ? 8 : 0), width,
                        wide_pop(cpu, width));
                break;
            case 0x69: case 0x6B: { /* IMUL r, r/m, immediate */
                const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                     rex_r, rex_x, rex_b, rex_w, rex_present);
                const uint64_t raw = read_rm(cpu, &modrm, width, rex_present);
                const int64_t source = width == 16 ? (int16_t)raw :
                                       width == 32 ? (int32_t)raw : (int64_t)raw;
                const int64_t immediate = opcode == 0x6B ? fetch_s8(cpu) :
                                          width == 16 ? fetch_s16(cpu) : fetch_s32(cpu);
                int64_t result;
                int overflow = __builtin_mul_overflow(source, immediate, &result);
                if (width == 16) overflow |= result != (int16_t)result;
                if (width == 32) overflow |= result != (int32_t)result;
                set_gpr(cpu, modrm.reg, width, (uint64_t)result);
                set_flag(cpu, X86_CF, overflow); set_flag(cpu, X86_OF, overflow);
                break;
            }
            case 0x60: { /* PUSHA/PUSHAD */
                if (cpu->mode == X86_MODE_64) {
                    cpu->rip = start_rip; wide_exception(cpu, 6, 0); break;
                }
                uint64_t original_sp = get_gpr(cpu, X86_RSP, width);
                for (int reg = 0; reg < 8; ++reg)
                    wide_push(cpu, width, reg == X86_RSP ? original_sp : get_gpr(cpu, reg, width));
                break;
            }
            case 0x61: { /* POPA/POPAD: discard saved stack pointer */
                if (cpu->mode == X86_MODE_64) {
                    cpu->rip = start_rip; wide_exception(cpu, 6, 0); break;
                }
                for (int reg = 7; reg >= 0; --reg) {
                    uint64_t value = wide_pop(cpu, width);
                    if (reg != X86_RSP) set_gpr(cpu, reg, width, value);
                }
                break;
            }
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
            case 0xC9: {                 /* LEAVE: SP <- BP; POP BP */
                const int stack_width = cpu->mode == X86_MODE_64 ? 64 :
                                        cpu->mode == X86_MODE_16 ? 16 : 32;
                set_gpr(cpu, X86_RSP, stack_width, get_gpr(cpu, X86_RBP, stack_width));
                set_gpr(cpu, X86_RBP, width, wide_pop(cpu, width));
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
            case 0xFE: { /* INC/DEC r/m8, preserving CF */
                const WideModRM modrm = decode_modrm(cpu, address_width, segment,
                                                     rex_r, rex_x, rex_b, rex_w, rex_present);
                if (modrm.reg > 1) {
                    cpu->rip = start_rip; wide_exception(cpu, 6, 0); break;
                }
                const int saved_cf = flag(cpu, X86_CF);
                const uint64_t value = read_rm(cpu, &modrm, 8, rex_present);
                const uint64_t result = wide_alu(cpu, modrm.reg ? 5 : 0, value, 1, 8);
                write_rm(cpu, &modrm, 8, rex_present, result);
                set_flag(cpu, X86_CF, saved_cf);
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
                    case 3: case 5: { /* CALL/JMP far m16:16 or m16:32 */
                        if (modrm.is_register || width == 64) {
                            cpu->rip = start_rip; wide_exception(cpu, 6, 0); break;
                        }
                        const uint64_t address = modrm_address(cpu, &modrm);
                        const uint16_t selector = (uint16_t)read_mem(cpu, address + width / 8, 16);
                        if (cpu->pf_pending) break;
                        if (modrm.reg == 3) {
                            wide_push(cpu, width, cpu->sregs[X86_CS]);
                            wide_push(cpu, width, cpu->rip);
                        }
                        load_descriptor(cpu, selector, value, opcode);
                        break;
                    }
                    case 4: cpu->rip = value; break;
                    case 6: wide_push(cpu, width, value); break;
                    default:
                        fault_wide(cpu, opcode, "Неподдерживаемый подкод группы FFh");
                        break;
                }
                break;
            }
            case 0xF4: cpu->halted = 1; break;
            case 0xF5: set_flag(cpu, X86_CF, !flag(cpu, X86_CF)); break;
            case 0xF8: set_flag(cpu, X86_CF, 0); break;
            case 0xF9: set_flag(cpu, X86_CF, 1); break;
            case 0xFA: set_flag(cpu, X86_IF, 0); cpu->sti_shadow=0; break;
            case 0xFB: set_flag(cpu, X86_IF, 1); cpu->sti_shadow=1; break;
            case 0xFC: set_flag(cpu, X86_DF, 0); break;
            case 0xFD: set_flag(cpu, X86_DF, 1); break;
            case 0x9C: wide_push(cpu, width, cpu->flags); break;
            case 0x9D: {
                uint32_t value = (uint32_t)wide_pop(cpu, width);
                if (width == 16) cpu->flags = (cpu->flags & 0xffff0000u) | (value & 0xffffu) | 2u;
                else cpu->flags = (value & 0x003f7fd7u) | 2u;
                break;
            }
            case 0x9E: cpu->flags = (cpu->flags & ~0xffu) | get_reg8(cpu, 4, rex_present); break;
            case 0x9F: set_reg8(cpu, 4, rex_present, (uint8_t)(cpu->flags & 0xFF)); break;
            case 0x98: { /* CBW/CWDE/CDQE */
                if (width==64) { int64_t v=(int32_t)get_gpr(cpu,X86_RAX,32); set_gpr(cpu,X86_RAX,64,(uint64_t)v); }
                else if (width==32) { int32_t v=(int16_t)get_gpr(cpu,X86_RAX,16); set_gpr(cpu,X86_RAX,32,(uint32_t)v); }
                else { int16_t v=(int8_t)get_reg8(cpu,0,rex_present); set_gpr(cpu,X86_RAX,16,(uint16_t)v); }
                break; }
            case 0x99: { /* CWD/CDQ/CQO */
                if (width==64) { int64_t v=(int64_t)get_gpr(cpu,X86_RAX,64); set_gpr(cpu,X86_RDX,64, v<0?~0ULL:0); }
                else if (width==32) { int32_t v=(int32_t)get_gpr(cpu,X86_RAX,32); set_gpr(cpu,X86_RDX,32, v<0?0xFFFFFFFFu:0); }
                else { int16_t v=(int16_t)get_gpr(cpu,X86_RAX,16); set_gpr(cpu,X86_RDX,16, v<0?0xFFFF:0); }
                break; }
            case 0xD7: { /* XLAT */
                uint64_t addr = linearize(cpu, get_gpr(cpu,X86_RBX,address_width) + get_reg8(cpu,0,rex_present), segment);
                uint8_t v = (uint8_t)read_mem(cpu, addr, 8);
                set_reg8(cpu, 0, rex_present, v);
                break; }
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: { /* LOOP/JCXZ */
                int32_t disp = fetch_s8(cpu);
                uint64_t cx = get_gpr(cpu, X86_RCX, address_width);
                if (opcode==0xE3) {
                    if (cx==0) cpu->rip = (uint64_t)((int64_t)cpu->rip + disp);
                } else {
                    cx = (cx - 1) & width_mask(address_width);
                    set_gpr(cpu, X86_RCX, address_width, cx);
                    int take = cx!=0;
                    if (opcode==0xE0) take = take && !flag(cpu,X86_ZF);
                    if (opcode==0xE1) take = take && flag(cpu,X86_ZF);
                    if (take) cpu->rip = (uint64_t)((int64_t)cpu->rip + disp);
                }
                break; }
            case 0xE4: case 0xE5: case 0xE6: case 0xE7: case 0xEC: case 0xED: case 0xEE: case 0xEF: {
                uint16_t port;
                if (opcode>=0xEC) port = (uint16_t)get_gpr(cpu,X86_RDX,16);
                else port = fetch8(cpu);
                int w = (opcode & 1) ? 16 : 8;
                if (opcode==0xE4||opcode==0xEC) set_reg8(cpu,0,rex_present, cpu->port_in?cpu->port_in(cpu->context,port):0xFF);
                else if (opcode==0xE5||opcode==0xED) set_gpr(cpu,X86_RAX,16, cpu->port_in?cpu->port_in(cpu->context,port):0xFF);
                else if (opcode==0xE6||opcode==0xEE) { if(cpu->port_out) cpu->port_out(cpu->context,port,get_reg8(cpu,0,rex_present)); }
                else { if(cpu->port_out){cpu->port_out(cpu->context,port,(uint8_t)get_gpr(cpu,X86_RAX,16)); cpu->port_out(cpu->context,(uint16_t)(port+1),(uint8_t)(get_gpr(cpu,X86_RAX,16)>>8));} }
                (void)w;
                break; }
            case 0x6C: case 0x6D: case 0x6E: case 0x6F: { /* INS/OUTS - simplified single */
                /* REP handling already stripped, do one iteration; REP prefix lost - guest using REP INS will loop via CX? Simplified: do CX iterations if F3 was present - we lost prefix info, do 1 */
                break; }
            case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
                int w = (opcode & 1) ? width : 8;
                int bytes = w/8;
                int step = flag(cpu,X86_DF) ? -bytes : bytes;
                uint64_t count = repetition ? get_gpr(cpu,X86_RCX,address_width) : 1;
                int continue_rep = repetition && count != 0;
                /* Bound each slice, preserving restartability and remaining count. */
                for (unsigned iter = 0; count && iter < 1024; ++iter) {
                    uint64_t si = get_gpr(cpu,X86_RSI,address_width);
                    uint64_t di = get_gpr(cpu,X86_RDI,address_width);
                    uint64_t src_addr = linearize(cpu, si, segment);
                    uint64_t dst_addr = linearize(cpu, di, X86_ES);
                    if (opcode==0xA4||opcode==0xA5) { uint64_t v=read_mem(cpu,src_addr,w); write_mem(cpu,dst_addr,w,v); }
                    else if (opcode==0xA6||opcode==0xA7) { uint64_t a=read_mem(cpu,src_addr,w); uint64_t b=read_mem(cpu,dst_addr,w); wide_alu(cpu,7,a,b,w); }
                    else if (opcode==0xAA||opcode==0xAB) { uint64_t v = w==8?get_reg8(cpu,0,rex_present):get_gpr(cpu,X86_RAX,w); write_mem(cpu,dst_addr,w,v); }
                    else if (opcode==0xAC||opcode==0xAD) { uint64_t v=read_mem(cpu,src_addr,w); if(w==8) set_reg8(cpu,0,rex_present,(uint8_t)v); else set_gpr(cpu,X86_RAX,w,v); }
                    else { uint64_t a = w==8?get_reg8(cpu,0,rex_present):get_gpr(cpu,X86_RAX,w); uint64_t b=read_mem(cpu,dst_addr,w); wide_alu(cpu,7,a,b,w); }
                    if (cpu->pf_pending) break;
                    if (opcode!=0xAA && opcode!=0xAB && opcode!=0xAE && opcode!=0xAF) set_gpr(cpu,X86_RSI,address_width, si+step);
                    if (opcode!=0xAC && opcode!=0xAD) set_gpr(cpu,X86_RDI,address_width, di+step);
                    --count;
                    if (repetition) set_gpr(cpu,X86_RCX,address_width,count);
                    if (repetition && (opcode==0xA6||opcode==0xA7||opcode==0xAE||opcode==0xAF)) {
                        int zf = flag(cpu,X86_ZF);
                        if ((repetition>0 && !zf) || (repetition<0 && zf)) {
                            continue_rep = 0;
                            break;
                        }
                    }
                }
                if (continue_rep && count && !cpu->pf_pending) cpu->rip = start_rip;
                break; }
            case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
                const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
                int w = (opcode & 1) ? width : 8;
                uint8_t count;
                if (opcode==0xC0||opcode==0xC1) count=fetch8(cpu);
                else if (opcode==0xD2||opcode==0xD3) count=(uint8_t)get_reg8(cpu,1,rex_present);
                else count=1;
                count &= (w==64?63:31);
                uint64_t val = read_rm(cpu,&modrm,w,rex_present);
                uint64_t res = val;
                int carry=0;
                if (count) {
                    switch(modrm.reg){
                        case 0: for(int i=0;i<count;i++){carry=(res>>(w-1))&1; res=((res<<1)|carry)&width_mask(w);} break;
                        case 1: for(int i=0;i<count;i++){carry=res&1; res=((res>>1)|(carry<<(w-1)))&width_mask(w);} break;
                        case 2: for(int i=0;i<count;i++){int oc=flag(cpu,X86_CF); carry=(res>>(w-1))&1; res=((res<<1)|oc)&width_mask(w); set_flag(cpu,X86_CF,carry);} break;
                        case 3: for(int i=0;i<count;i++){int oc=flag(cpu,X86_CF); carry=res&1; res=((res>>1)|(oc<<(w-1)))&width_mask(w); set_flag(cpu,X86_CF,carry);} break;
                        case 4: case 6: for(int i=0;i<count;i++){carry=(res>>(w-1))&1; res=(res<<1)&width_mask(w);} set_flag(cpu,X86_CF,carry); break;
                        case 5: for(int i=0;i<count;i++){carry=res&1; res>>=1;} set_flag(cpu,X86_CF,carry); break;
                        default: for(int i=0;i<count;i++){carry=res&1; res=((int64_t)res>>1) | (res & (1ULL<<(w-1)));} set_flag(cpu,X86_CF,carry); break;
                    }
                    if (modrm.reg>=4){ set_flag(cpu,X86_ZF,res==0); set_flag(cpu,X86_SF,(res>>(w-1))&1); set_flag(cpu,X86_PF,parity((uint8_t)res)); }
                }
                write_rm(cpu,&modrm,w,rex_present,res);
                break; }
            case 0xF6: case 0xF7: {
                const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
                int w = (opcode & 1) ? width : 8;
                uint64_t val = read_rm(cpu,&modrm,w,rex_present);
                if (modrm.reg==0) { uint64_t imm = w==8?fetch8(cpu): w==16?fetch16(cpu): fetch32(cpu); wide_alu(cpu,4,val,imm,w); }
                else if (modrm.reg==2) { write_rm(cpu,&modrm,w,rex_present, (~val)&width_mask(w)); }
                else if (modrm.reg==3) { uint64_t r=wide_alu(cpu,5,0,val,w); write_rm(cpu,&modrm,w,rex_present,r); }
                else if (modrm.reg==4) { /* MUL */
                    if (w==8){ uint32_t p=(uint32_t)get_reg8(cpu,0,rex_present)* (uint32_t)val; set_gpr(cpu,X86_RAX,16,p); set_flag(cpu,X86_CF,p>0xFF); set_flag(cpu,X86_OF,p>0xFF); }
                    else if (w==32){ uint64_t p=get_gpr(cpu,X86_RAX,32)*val; set_gpr(cpu,X86_RAX,32,(uint32_t)p); set_gpr(cpu,X86_RDX,32,(uint32_t)(p>>32)); set_flag(cpu,X86_CF, get_gpr(cpu,X86_RDX,32)!=0); set_flag(cpu,X86_OF, get_gpr(cpu,X86_RDX,32)!=0); }
                    else if (w==64){ /* 128-bit mul simplified low */
                        uint64_t lo = get_gpr(cpu,X86_RAX,64)*val;
                        set_gpr(cpu,X86_RAX,64,lo);
                        set_gpr(cpu,X86_RDX,64,0);
                        set_flag(cpu,X86_CF,0); set_flag(cpu,X86_OF,0);
                    } else { uint32_t p=(uint32_t)get_gpr(cpu,X86_RAX,16)*(uint32_t)val; set_gpr(cpu,X86_RAX,16,(uint16_t)p); set_gpr(cpu,X86_RDX,16,(uint16_t)(p>>16)); set_flag(cpu,X86_CF, get_gpr(cpu,X86_RDX,16)!=0); set_flag(cpu,X86_OF, get_gpr(cpu,X86_RDX,16)!=0); }
                }
                else if (modrm.reg==5) { /* IMUL */
                    if (w==8){ int32_t p=(int32_t)(int8_t)get_reg8(cpu,0,rex_present)*(int32_t)(int8_t)val; set_gpr(cpu,X86_RAX,16,(uint16_t)p); int ov = p!=(int8_t)p; set_flag(cpu,X86_CF,ov); set_flag(cpu,X86_OF,ov); }
                    else if (w==32){ int64_t p=(int64_t)(int32_t)get_gpr(cpu,X86_RAX,32)*(int64_t)(int32_t)val; set_gpr(cpu,X86_RAX,32,(uint32_t)p); set_gpr(cpu,X86_RDX,32,(uint32_t)(p>>32)); int ov = p!=(int32_t)p; set_flag(cpu,X86_CF,ov); set_flag(cpu,X86_OF,ov); }
                    else if (w==64){ int64_t p=(int64_t)get_gpr(cpu,X86_RAX,64)*(int64_t)val; set_gpr(cpu,X86_RAX,64,(uint64_t)p); set_gpr(cpu,X86_RDX,64,0); set_flag(cpu,X86_CF,0); set_flag(cpu,X86_OF,0); }
                    else { int32_t p=(int32_t)(int16_t)get_gpr(cpu,X86_RAX,16)*(int32_t)(int16_t)val; set_gpr(cpu,X86_RAX,16,(uint16_t)p); set_gpr(cpu,X86_RDX,16,(uint16_t)(p>>16)); int ov=p!=(int16_t)p; set_flag(cpu,X86_CF,ov); set_flag(cpu,X86_OF,ov); }
                }
                else { /* DIV/IDIV */
                    if (val==0){ cpu->rip=start_rip; wide_exception(cpu,0,0); break; }
                    if (w==8){ uint32_t dvd=get_gpr(cpu,X86_RAX,16); uint32_t q=dvd/val; uint32_t r=dvd%val; if(q>0xFF){cpu->rip=start_rip; wide_exception(cpu,0,0); break;} set_reg8(cpu,0,rex_present,(uint8_t)q); set_reg8(cpu,4,rex_present,(uint8_t)r); }
                    else if (w==32){ uint64_t dvd=(get_gpr(cpu,X86_RDX,32)<<32)|get_gpr(cpu,X86_RAX,32); uint64_t q=dvd/val; uint64_t r=dvd%val; if(q>0xFFFFFFFFULL){cpu->rip=start_rip; wide_exception(cpu,0,0); break;} set_gpr(cpu,X86_RAX,32,(uint32_t)q); set_gpr(cpu,X86_RDX,32,(uint32_t)r); }
                    else if (w==64){ /* 128/64 simplified: use low */
                        uint64_t dvd=get_gpr(cpu,X86_RAX,64); if(val==0){cpu->rip=start_rip; wide_exception(cpu,0,0); break;} uint64_t q=dvd/val; uint64_t r=dvd%val; set_gpr(cpu,X86_RAX,64,q); set_gpr(cpu,X86_RDX,64,r);
                    } else { uint32_t dvd=(get_gpr(cpu,X86_RDX,16)<<16)|get_gpr(cpu,X86_RAX,16); uint32_t q=dvd/val; uint32_t r=dvd%val; if(q>0xFFFF){cpu->rip=start_rip; wide_exception(cpu,0,0); break;} set_gpr(cpu,X86_RAX,16,(uint16_t)q); set_gpr(cpu,X86_RDX,16,(uint16_t)r); }
                }
                break; }
            case 0x86: case 0x87: {
                const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
                int w = (opcode & 1) ? width : 8;
                uint64_t a = read_rm(cpu,&modrm,w,rex_present);
                uint64_t b = w==8?get_reg8(cpu,modrm.reg,rex_present):get_gpr(cpu,modrm.reg,w);
                write_rm(cpu,&modrm,w,rex_present,b);
                if(w==8) set_reg8(cpu,modrm.reg,rex_present,(uint8_t)a); else set_gpr(cpu,modrm.reg,w,a);
                break; }
            case 0x8C: case 0x8E: {
                const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
                int idx = modrm.reg;
                if (idx >= 6 || (opcode == 0x8E && idx == X86_CS)) {
                    cpu->rip = start_rip; wide_exception(cpu, 6, 0); break;
                }
                if (opcode==0x8C) write_rm(cpu,&modrm,16,rex_present,cpu->seg_sel[idx]);
                else { uint16_t v=(uint16_t)read_rm(cpu,&modrm,16,rex_present); if (x86_load_segment(cpu,idx,v)!=0){cpu->rip=start_rip; wide_exception(cpu,13,v); break;} if(idx==X86_SS) cpu->mov_ss_shadow=1; }
                break; }
            case 0xC4: case 0xC5: { /* LES/LDS in wide - actually LSS/LFS handled in 0F, these are LES/LDS */
                const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
                if (modrm.is_register){cpu->rip=start_rip; wide_exception(cpu,13,0); break;}
                uint64_t addr=modrm_address(cpu,&modrm);
                uint64_t off=read_mem(cpu,addr,width==16?16:32);
                uint16_t sel=(uint16_t)read_mem(cpu,addr+(width==16?2:4),16);
                set_gpr(cpu,modrm.reg,width,off);
                x86_load_segment(cpu, opcode==0xC4?X86_ES:X86_DS, sel);
                break; }
            case 0xCA: { uint16_t imm=fetch16(cpu); cpu->rip=wide_pop(cpu,width); uint16_t cs=(uint16_t)wide_pop(cpu,width); x86_load_segment(cpu,X86_CS,cs); set_gpr(cpu,X86_RSP,width==64?64:32, get_gpr(cpu,X86_RSP,width==64?64:32)+imm); break; }
            case 0xCB: { cpu->rip=wide_pop(cpu,width); uint16_t cs=(uint16_t)wide_pop(cpu,width); x86_load_segment(cpu,X86_CS,cs); break; }
            case 0xCC: x86_deliver_interrupt(cpu,3,0,0); break;
            case 0xCD: {
                const uint8_t vector = fetch8(cpu);
                if (!(cpu->cr0 & X86_CR0_PE)) {
                    x86_wide_sync_to_real(cpu);
                    if (cpu->interrupt_hook && cpu->interrupt_hook(cpu->context, vector)) {
                        x86_wide_sync_from_real(cpu);
                    } else {
                        x86_deliver_interrupt(cpu, vector, 0, 0);
                    }
                } else {
                    x86_deliver_interrupt(cpu, vector, 0, 0);
                }
                break;
            }
            case 0xCE: if (flag(cpu,X86_OF)) x86_deliver_interrupt(cpu,4,0,0); break;
            case 0xCF: { /* IRET */
                if (cpu->mode==X86_MODE_16) {
                    cpu->rip=wide_pop(cpu,16); uint16_t cs=(uint16_t)wide_pop(cpu,16); x86_load_segment(cpu,X86_CS,cs); cpu->flags=(uint16_t)wide_pop(cpu,16);
                } else if (cpu->mode==X86_MODE_32) {
                    cpu->rip=wide_pop(cpu,32); uint16_t cs=(uint16_t)wide_pop(cpu,32); x86_load_segment(cpu,X86_CS,cs); cpu->flags=(uint16_t)wide_pop(cpu,32);
                } else {
                    cpu->rip=wide_pop(cpu,64); uint16_t cs=(uint16_t)wide_pop(cpu,64); x86_load_segment(cpu,X86_CS,cs); cpu->flags=(uint16_t)wide_pop(cpu,32);
                    /* check CS.RPL vs CPL for ring transition - simplified */
                }
                break; }
            case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
                /* x87: minimal - FLD/FSTP etc as NOP, consume modrm */
                const WideModRM modrm = decode_modrm(cpu, address_width, segment, rex_r, rex_x, rex_b, rex_w, rex_present);
                (void)modrm;
                break; }
            case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
                /* MOV accumulator <-> absolute offset (moffs), whose width
                 * follows the address-size attribute, not the operand size. */
                uint64_t offset = address_width == 16 ? fetch16(cpu) :
                                  address_width == 32 ? fetch32(cpu) : fetch64(cpu);
                uint64_t address = linearize(cpu, offset, segment);
                int data_width = (opcode == 0xA0 || opcode == 0xA2) ? 8 : width;
                if (opcode == 0xA0 || opcode == 0xA1) {
                    uint64_t value = read_mem(cpu, address, data_width);
                    if (data_width == 8) set_reg8(cpu, 0, rex_present, (uint8_t)value);
                    else set_gpr(cpu, X86_RAX, data_width, value);
                } else {
                    uint64_t value = data_width == 8 ? get_reg8(cpu, 0, rex_present)
                                                     : get_gpr(cpu, X86_RAX, data_width);
                    write_mem(cpu, address, data_width, value);
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
                cpu->rip = start_rip;
                wide_exception(cpu, 6, 0);
                break;
        }
    }

    if (cpu->pf_pending) {
        cpu->pf_pending = 0;
        cpu->rip = start_rip;
        x86_wide_sync_to_real(cpu);
        wide_exception(cpu, 14, cpu->pf_error);
        if (cpu->mode == X86_MODE_16) cpu->ip = (uint16_t)cpu->rip;
        return 1;
    }
    x86_wide_sync_to_real(cpu);
    if (cpu->waiting) {
        cpu->rip = start_rip;
    }
    if (cpu->mode == X86_MODE_16) {
        cpu->ip = (uint16_t)cpu->rip;
    }
    return cpu->halted || cpu->waiting || cpu->fault ? 0 : 1;
}
