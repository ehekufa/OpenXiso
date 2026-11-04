#ifndef X86_CPU_H
#define X86_CPU_H

/*
 * x86_cpu.h — интерпретатор реального режима 8086/186.
 *
 * Это не имитация: ядро исполняет настоящие машинные коды загрузочного сектора
 * из плоской 1 МБ памяти, работает с сегментами, флагами, стеком, строковыми
 * операциями, портами и прерываниями. Обращения к портам передаются «железу»
 * (PcMachine), а вызовы INT обрабатывает собственный Legacy BIOS.
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
    X86_ES = 0, X86_CS, X86_SS, X86_DS
};

#define X86_CF 0x0001u
#define X86_PF 0x0004u
#define X86_AF 0x0010u
#define X86_ZF 0x0040u
#define X86_SF 0x0080u
#define X86_TF 0x0100u
#define X86_IF 0x0200u
#define X86_DF 0x0400u
#define X86_OF 0x0800u

typedef struct X86Cpu X86Cpu;

typedef uint8_t (*X86PortIn)(void* context, uint16_t port);
typedef void (*X86PortOut)(void* context, uint16_t port, uint8_t value);

struct X86Cpu {
    uint16_t regs[8];
    uint16_t sregs[4];
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

#ifdef __cplusplus
}
#endif

#endif /* X86_CPU_H */
