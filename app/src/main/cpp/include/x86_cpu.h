#ifndef X86_CPU_H
#define X86_CPU_H

/*
 * x86_cpu.h — интерпретатор x86: реальный режим 8086/186, защищённый режим
 * i386 и long mode x86-64.
 *
 * Это не имитация: ядро исполняет настоящие машинные коды загрузочного сектора
 * и ядра, работает с сегментами, флагами, стеком, строковыми операциями,
 * портами и прерываниями. Обращения к портам передаются «железу» (PcMachine),
 * а вызовы INT обрабатывает собственный Legacy BIOS.
 *
 * Разрядность гостя задаётся через max_mode: это аналог выбора процессора
 * (8086 / i386 / x86-64). Машина ВСЕГДА стартует в реальном режиме, как
 * настоящий PC, а гость сам переключается в 32/64 бита через CR0 и EFER.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    X86_AX = 0, X86_CX, X86_DX, X86_BX,
    X86_SP, X86_BP, X86_SI, X86_DI
};

enum {
    X86_ES = 0, X86_CS, X86_SS, X86_DS, X86_FS, X86_GS
};

/* Регистры широкого ядра: RAX..R15. */
enum {
    X86_RAX = 0, X86_RCX, X86_RDX, X86_RBX,
    X86_RSP, X86_RBP, X86_RSI, X86_RDI,
    X86_R8, X86_R9, X86_R10, X86_R11,
    X86_R12, X86_R13, X86_R14, X86_R15
};

typedef enum X86Mode {
    X86_MODE_16 = 0,     /* реальный режим 8086/186 */
    X86_MODE_32 = 1,     /* защищённый режим i386   */
    X86_MODE_64 = 2      /* long mode x86-64        */
} X86Mode;

#define X86_CF 0x0001u
#define X86_PF 0x0004u
#define X86_AF 0x0010u
#define X86_ZF 0x0040u
#define X86_SF 0x0080u
#define X86_TF 0x0100u
#define X86_IF 0x0200u
#define X86_DF 0x0400u
#define X86_OF 0x0800u

#define X86_CR0_PE 0x00000001u
#define X86_CR0_PG 0x80000000u
#define X86_EFER_LME 0x00000100u
#define X86_MSR_EFER 0xC0000080u

typedef struct X86Cpu X86Cpu;

typedef uint8_t (*X86PortIn)(void* context, uint16_t port);
typedef void (*X86PortOut)(void* context, uint16_t port, uint8_t value);

struct X86Cpu {
    /* --- Классическое 16-битное состояние (используется BIOS и POST) --- */
    uint16_t regs[8];
    uint16_t sregs[6];
    uint16_t ip;
    uint16_t flags;

    uint8_t* memory;
    size_t memory_size;

    /* halted — ядро остановлено; waiting — ожидание клавиши, IP отмотан. */
    int halted;
    int waiting;
    int fault;
    uint8_t fault_opcode;
    uint64_t instructions;

    void* context;
    X86PortIn port_in;
    X86PortOut port_out;

    /*
     * Firmware-хук прерываний: возвращает 1, если BIOS обработал INT сам.
     * Позволяет прошивке менять регистры и CF без искажения кадра iret.
     */
    int (*interrupt_hook)(void* context, uint8_t vector);

    /* --- Состояние 32/64-битного ядра --- */
    X86Mode mode;              /* текущий режим исполнения */
    X86Mode max_mode;          /* предельная разрядность гостя */
    uint64_t gpr[16];          /* RAX..R15 */
    uint64_t rip;              /* смещение внутри CS (в 16-битах) либо линейный адрес */
    uint32_t cr0;
    uint32_t cr3;
    uint32_t cr4;
    uint64_t efer;
    uint64_t gdtr_base;
    uint16_t gdtr_limit;
    uint64_t idtr_base;
    uint16_t idtr_limit;
    uint64_t seg_base[6];      /* базы сегментов в плоской памяти */
    uint32_t seg_limit[6];
    uint16_t seg_sel[6];       /* селекторы в защищённом режиме */
    int a20_enabled;
    int wide_ready;            /* состояние wide-ядра уже синхронизировано */
    const char* fault_reason;  /* уточнение на русском, если переход запрещён */
};

void x86_reset(X86Cpu* cpu, uint8_t* memory, size_t memory_size);

/** Исполняет одну инструкцию. Возвращает 1 при успехе, 0 при останове/ожидании. */
int x86_step(X86Cpu* cpu);

/** Программное прерывание: пушит FLAGS/CS/IP и читает вектор из IVT. */
void x86_raise_interrupt(X86Cpu* cpu, uint8_t vector);

uint32_t x86_physical(uint16_t segment, uint16_t offset);
uint8_t x86_read_byte(const X86Cpu* cpu, uint32_t physical);
void x86_write_byte(X86Cpu* cpu, uint32_t physical, uint8_t value);
uint16_t x86_read_word(const X86Cpu* cpu, uint32_t physical);
void x86_write_word(X86Cpu* cpu, uint32_t physical, uint16_t value);

void x86_set_carry(X86Cpu* cpu, int value);
void x86_set_zero(X86Cpu* cpu, int value);
void x86_push(X86Cpu* cpu, uint16_t value);
uint16_t x86_pop(X86Cpu* cpu);

/* ---------------- Управление разрядностью ---------------- */

void x86_set_max_mode(X86Cpu* cpu, X86Mode mode);
X86Mode x86_get_mode(const X86Cpu* cpu);
X86Mode x86_get_max_mode(const X86Cpu* cpu);
const char* x86_mode_name(X86Mode mode);
const char* x86_mode_description(X86Mode mode);
uint64_t x86_wide_ip(const X86Cpu* cpu);

/* Связь 16-битного и широкого регистровых файлов. */
void x86_wide_sync_from_real(X86Cpu* cpu);
void x86_wide_sync_to_real(X86Cpu* cpu);

/* Широкий декодер: вызывается из x86_step при 0x66/0x67/0x0F и в PM/LM. */
int x86_wide_step(X86Cpu* cpu);

#ifdef __cplusplus
}
#endif

#endif /* X86_CPU_H */
