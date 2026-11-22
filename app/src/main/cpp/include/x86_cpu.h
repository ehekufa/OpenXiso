#ifndef X86_CPU_H
#define X86_CPU_H

/*
 * x86_cpu.h — интерпретатор x86: реальный режим 8086/186, защищённый режим
 * i386 и long mode x86-64.
 *
 * Реализованы: сегменты/GDT/IDT, страничная память (32-бит, PAE, 4-level),
 * исключения (#DE/#GP/#PF/#UD/#SS...), аппаратные IRQ через PIC/APIC,
 * MSR (EFER/STAR/LSTAR/FS_BASE/GS_BASE/TSC/PAT...), SYSCALL/SYSRET,
 * RDTSC, полный целочисленный набор wide-ядра, базовый SSE/SSE2 для
 * MOVAPS/MOVUPS/MOVDQA/PXOR и т.д., FXSAVE/FXRSTOR, x87-заглушка.
 * Обращения к портам и MMIO передаются «железу» (PcMachine).
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

enum {
    X86_RAX = 0, X86_RCX, X86_RDX, X86_RBX,
    X86_RSP, X86_RBP, X86_RSI, X86_RDI,
    X86_R8, X86_R9, X86_R10, X86_R11,
    X86_R12, X86_R13, X86_R14, X86_R15
};

typedef enum X86Mode {
    X86_MODE_16 = 0,
    X86_MODE_32 = 1,
    X86_MODE_64 = 2
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
#define X86_RF 0x10000u
#define X86_VM 0x20000u

#define X86_CR0_PE 0x00000001u
#define X86_CR0_MP 0x00000002u
#define X86_CR0_EM 0x00000004u
#define X86_CR0_TS 0x00000008u
#define X86_CR0_ET 0x00000010u
#define X86_CR0_NE 0x00000020u
#define X86_CR0_WP 0x00010000u
#define X86_CR0_AM 0x00040000u
#define X86_CR0_NW 0x20000000u
#define X86_CR0_CD 0x40000000u
#define X86_CR0_PG 0x80000000u

#define X86_CR4_PSE 0x00000010u
#define X86_CR4_PAE 0x00000020u
#define X86_CR4_MCE 0x00000040u
#define X86_CR4_PGE 0x00000080u
#define X86_CR4_OSFXSR 0x00000200u
#define X86_CR4_OSXMMEXCPT 0x00000400u
#define X86_CR4_FSGSBASE 0x00010000u
#define X86_CR4_PKE 0x00020000u

#define X86_EFER_SCE 0x00000001u
#define X86_EFER_LME 0x00000100u
#define X86_EFER_LMA 0x00000400u
#define X86_EFER_NXE 0x00000800u
#define X86_EFER_FFXSR 0x00004000u

#define X86_MSR_EFER 0xC0000080u
#define X86_MSR_STAR 0xC0000081u
#define X86_MSR_LSTAR 0xC0000082u
#define X86_MSR_FMASK 0xC0000083u
#define X86_MSR_FS_BASE 0xC0000100u
#define X86_MSR_GS_BASE 0xC0000101u
#define X86_MSR_KERNEL_GS_BASE 0xC0000102u
#define X86_MSR_TSC_AUX 0xC0000103u
#define X86_MSR_PAT 0x00000277u
#define X86_MSR_TSC 0x00000010u
#define X86_MSR_APIC_BASE 0x0000001Bu
#define X86_MSR_SYSENTER_CS 0x00000179u
#define X86_MSR_SYSENTER_ESP 0x0000017Au
#define X86_MSR_SYSENTER_EIP 0x0000017Bu

typedef struct X86Cpu X86Cpu;

typedef uint8_t (*X86PortIn)(void* context, uint16_t port);
typedef void (*X86PortOut)(void* context, uint16_t port, uint8_t value);
typedef uint64_t (*X86MmioRead)(void* context, uint64_t addr, int width);
typedef void (*X86MmioWrite)(void* context, uint64_t addr, int width, uint64_t value);
typedef int (*X86MmioCheck)(void* context, uint64_t addr);
typedef int (*X86IrqCheck)(void* context); /* возвращает вектор IRQ или -1 */

struct X86Cpu {
    uint16_t regs[8];
    uint16_t sregs[6];
    uint16_t ip;
    uint32_t flags;

    uint8_t* memory;
    size_t memory_size;

    int halted;
    int waiting;
    int fault;
    uint8_t fault_opcode;
    uint64_t instructions;

    void* context;
    X86PortIn port_in;
    X86PortOut port_out;
    X86MmioRead mmio_read;
    X86MmioWrite mmio_write;
    X86MmioCheck mmio_check;
    X86IrqCheck irq_check;

    int (*interrupt_hook)(void* context, uint8_t vector);

    X86Mode mode;
    X86Mode max_mode;
    uint64_t gpr[16];
    uint64_t rip;
    uint32_t cr0;
    uint32_t cr3;
    uint32_t cr4;
    uint32_t cr8;
    uint64_t cr2;
    uint64_t efer;
    uint64_t gdtr_base;
    uint16_t gdtr_limit;
    uint64_t idtr_base;
    uint16_t idtr_limit;
    uint64_t seg_base[6];
    uint32_t seg_limit[6];
    uint16_t seg_sel[6];
    uint8_t seg_attr[6]; /* access byte */
    uint8_t seg_flags[6]; /* G/D/B/L/AVL */
    int a20_enabled;
    int wide_ready;
    const char* fault_reason;
    int cpl;
    int sti_shadow;
    int mov_ss_shadow;
    uint64_t tr_base;
    uint32_t tr_limit;
    uint16_t tr_sel;
    uint64_t ldtr_base;
    uint32_t ldtr_limit;
    uint16_t ldtr_sel;
    uint64_t msr_star;
    uint64_t msr_lstar;
    uint64_t msr_fmask;
    uint64_t msr_fs_base;
    uint64_t msr_gs_base;
    uint64_t msr_kernel_gs_base;
    uint64_t msr_tsc_aux;
    uint64_t msr_pat;
    uint64_t msr_sysenter_cs;
    uint64_t msr_sysenter_esp;
    uint64_t msr_sysenter_eip;
    uint64_t tsc;
    uint64_t apic_base;
    uint8_t xmm[16][16];
    uint8_t x87_stack[8][10];
    int x87_top;
    uint16_t x87_cw;
    uint16_t x87_sw;
    uint64_t dr[8];
    int triple_fault;
    uint64_t last_linear;
    int in_interrupt; /* защита от рекурсии при доставке */
    int pf_pending;
    uint32_t pf_error;
    uint64_t pf_linear;
    uint64_t instr_start_rip;
    int instr_abort;
};

void x86_reset(X86Cpu* cpu, uint8_t* memory, size_t memory_size);
int x86_step(X86Cpu* cpu);
void x86_raise_interrupt(X86Cpu* cpu, uint8_t vector);
void x86_deliver_interrupt(X86Cpu* cpu, uint8_t vector, uint32_t error_code, int has_error);
void x86_deliver_exception(X86Cpu* cpu, uint8_t vector, uint32_t error_code);
int x86_check_and_deliver_irq(X86Cpu* cpu);

uint32_t x86_physical(uint16_t segment, uint16_t offset);
uint32_t x86_physical_a20(const X86Cpu* cpu, uint16_t segment, uint16_t offset);
uint8_t x86_read_byte(const X86Cpu* cpu, uint32_t physical);
void x86_write_byte(X86Cpu* cpu, uint32_t physical, uint8_t value);
uint16_t x86_read_word(const X86Cpu* cpu, uint32_t physical);
void x86_write_word(X86Cpu* cpu, uint32_t physical, uint16_t value);

void x86_set_carry(X86Cpu* cpu, int value);
void x86_set_zero(X86Cpu* cpu, int value);
void x86_push(X86Cpu* cpu, uint16_t value);
uint16_t x86_pop(X86Cpu* cpu);

void x86_set_max_mode(X86Cpu* cpu, X86Mode mode);
X86Mode x86_get_mode(const X86Cpu* cpu);
X86Mode x86_get_max_mode(const X86Cpu* cpu);
const char* x86_mode_name(X86Mode mode);
const char* x86_mode_description(X86Mode mode);
uint64_t x86_wide_ip(const X86Cpu* cpu);

void x86_wide_sync_from_real(X86Cpu* cpu);
void x86_wide_sync_to_real(X86Cpu* cpu);
int x86_wide_step(X86Cpu* cpu);

/* helpers for wide core */
uint64_t x86_read_physical(X86Cpu* cpu, uint64_t addr, int width);
void x86_write_physical(X86Cpu* cpu, uint64_t addr, int width, uint64_t value);
/* pf_error is -1 on success, otherwise the architectural page-fault error code. */
uint64_t x86_translate(X86Cpu* cpu, uint64_t linear, int is_write, int is_fetch, int* pf_error);
int x86_load_segment(X86Cpu* cpu, int seg_index, uint16_t selector);
uint64_t x86_read_msr(X86Cpu* cpu, uint32_t msr);
void x86_write_msr(X86Cpu* cpu, uint32_t msr, uint64_t value);

#ifdef __cplusplus
}
#endif

#endif
