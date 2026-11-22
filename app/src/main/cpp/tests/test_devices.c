#define _POSIX_C_SOURCE 200809L
#include "pc_machine.h"
#include "legacy_bios.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static void test_eltorito_status(PcMachine* m) {
    x86_reset(&m->cpu, m->memory, m->memory_size);
    memset(&m->plan, 0, sizeof(m->plan));
    m->plan.el_torito = 1;
    m->plan.emulation = OPENXISO_EMULATION_NONE;
    m->plan.drive = 0xe0;
    m->plan.boot_lba = 0x12345678;
    m->plan.boot_sectors = 4;
    m->cpu.sregs[X86_DS] = 0;
    m->cpu.regs[X86_SI] = 0x600;
    memset(m->memory + 0x600, 0xcc, 20);
    m->cpu.regs[X86_AX] = 0x4b01;
    m->cpu.regs[X86_DX] = 0xe0;
    m->cpu.flags |= X86_CF;
    legacy_bios_interrupt(m, 0x13);
    CHECK(!(m->cpu.flags & X86_CF));
    CHECK((m->cpu.regs[X86_AX] >> 8) == 0);
    CHECK(m->memory[0x600] == 0x13 && m->memory[0x601] == 0 && m->memory[0x602] == 0xe0);
    CHECK(x86_read_physical(&m->cpu, 0x604, 32) == 0x12345678);
    CHECK(x86_read_physical(&m->cpu, 0x60e, 16) == 4 && m->memory[0x613] == 0xcc);
    m->cpu.regs[X86_AX] = 0x4b01;
    m->cpu.regs[X86_DX] = 0x80;
    legacy_bios_interrupt(m, 0x13);
    CHECK(m->cpu.flags & X86_CF);
    CHECK(m->memory[0x602] == 0xe0);
    m->cpu.regs[X86_AX] = 0x4b00; /* termination is not implemented */
    m->cpu.regs[X86_DX] = 0xe0;
    legacy_bios_interrupt(m, 0x13);
    CHECK(m->cpu.flags & X86_CF);
    m->plan.el_torito = 0;
    m->cpu.regs[X86_AX] = 0x4b01;
    legacy_bios_interrupt(m, 0x13);
    CHECK(m->cpu.flags & X86_CF);
}

static void test_eltorito_load_segment(PcMachine* m) {
    FILE* f = tmpfile();
    CHECK(f != NULL);
    if (!f) return;
    uint8_t data[10 * 512];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i / 512);
    CHECK(fwrite(data, 1, sizeof(data), f) == sizeof(data));
    CHECK(fflush(f) == 0);
    x86_reset(&m->cpu, m->memory, m->memory_size);
    memset(&m->plan, 0, sizeof(m->plan));
    m->plan.el_torito = 1;
    m->plan.boot_sectors = 10;
    m->plan.load_segment = 0x1000;
    m->plan.drive = 0xe0;
    int previous_fd = m->fd;
    m->fd = fileno(f);
    memset(m->memory + 0x10000, 0xff, sizeof(data));
    legacy_bios_interrupt(m, 0x19);
    CHECK(m->cpu.sregs[X86_CS] == 0x1000 && m->cpu.ip == 0);
    CHECK(!memcmp(m->memory + 0x10000, data, sizeof(data)));
    m->fd = previous_fd;
    fclose(f);
}

static void test_unprefixed_protected_far_jump(PcMachine* m) {
    X86Cpu* c = &m->cpu;
    x86_reset(c, m->memory, m->memory_size);
    x86_set_max_mode(c, X86_MODE_32);
    c->sregs[X86_CS] = c->sregs[X86_DS] = c->sregs[X86_SS] = 0;
    c->ip = 0x500; c->regs[X86_SP] = 0x800;
    const uint8_t descriptor[] = {0xff,0xff,0,0,0,0x9a,0xcf,0};
    memcpy(m->memory + 0x1008, descriptor, sizeof(descriptor));
    const uint8_t gdtr[] = {0x0f, 0, 0x00, 0x10, 0, 0xff};
    memcpy(m->memory + 0xaa90, gdtr, sizeof(gdtr));
    const uint8_t code[] = {0x0f,0x01,0x16,0x90,0xaa, /* LGDT [aa90] */
                            0x66,0xb8,1,0,0,0, 0x0f,0x22,0xc0,
                            0xea,0x50,0x06,0x08,0x00};
    memcpy(m->memory + 0x500, code, sizeof(code));
    m->memory[0x650] = 0xf4;
    CHECK(x86_step(c));
    CHECK(c->gdtr_base == 0x1000 && c->gdtr_limit == 0x0f);
    CHECK(x86_step(c)); CHECK(x86_step(c));
    CHECK((c->cr0 & X86_CR0_PE) && c->mode == X86_MODE_16);
    CHECK(x86_step(c));
    CHECK(!c->fault && !c->triple_fault && c->mode == X86_MODE_32);
    CHECK(c->seg_sel[X86_CS] == 8 && c->rip == 0x650 && c->seg_base[X86_CS] == 0);
    CHECK(!x86_step(c) && c->halted);
}

static void test_return_to_real_cs_cache(PcMachine* m) {
    X86Cpu* c = &m->cpu;
    x86_reset(c, m->memory, m->memory_size);
    x86_set_max_mode(c, X86_MODE_32);
    c->wide_ready = 1; c->mode = X86_MODE_16; c->cr0 = X86_CR0_PE;
    c->sregs[X86_CS] = c->seg_sel[X86_CS] = 0x10;
    c->seg_base[X86_CS] = 0; c->seg_limit[X86_CS] = 0xffff;
    c->ip = c->rip = 0x500; c->gpr[X86_RAX] = 0;
    /* clear PE; far jump must be fetched at cached base 0, not 0x100 */
    const uint8_t code[] = {0x0f,0x22,0xc0,0xea,0x50,0x06,0,0};
    memcpy(m->memory + 0x500, code, sizeof(code));
    memset(m->memory + 0x600, 0xf4, sizeof(code));
    m->memory[0x650] = 0xf4;
    CHECK(x86_step(c));
    CHECK(!(c->cr0 & X86_CR0_PE) && c->ip == 0x503);
    CHECK(x86_step(c));
    CHECK(c->ip == 0x650 && c->sregs[X86_CS] == 0 && c->seg_base[X86_CS] == 0);
    CHECK(x86_step(c) && c->halted);
}

static void test_address32_indirect_far_call(PcMachine* m) {
    X86Cpu* c = &m->cpu;
    x86_reset(c, m->memory, m->memory_size);
    x86_set_max_mode(c, X86_MODE_32);
    c->sregs[X86_CS] = c->sregs[X86_DS] = c->sregs[X86_SS] = 0;
    c->ip = 0x500; c->regs[X86_SP] = 0x900; c->regs[X86_SI] = 1;
    const uint8_t code[] = {0x67,0xff,0x1c,0xb5,0x00,0x10,0x00,0x00,0xf4};
    const uint8_t target[] = {0xb8,0x34,0x12,0xcb};
    memcpy(m->memory + 0x500, code, sizeof(code));
    memcpy(m->memory + 0x700, target, sizeof(target));
    x86_write_word(c, 0x1004, 0x700); x86_write_word(c, 0x1006, 0);
    CHECK(x86_step(c)); CHECK(c->ip == 0x700 && c->regs[X86_SP] == 0x8fc);
    CHECK(x86_read_word(c, 0x8fc) == 0x508 && x86_read_word(c, 0x8fe) == 0);
    CHECK(x86_step(c)); CHECK(c->regs[X86_AX] == 0x1234);
    CHECK(x86_step(c)); CHECK(c->ip == 0x508 && c->regs[X86_SP] == 0x900);
    CHECK(x86_step(c) && c->halted);
}

static void test_ldtr_and_tr(PcMachine* m) {
    X86Cpu* c = &m->cpu;
    x86_reset(c, m->memory, m->memory_size);
    x86_set_max_mode(c, X86_MODE_32);
    c->wide_ready = 1; c->mode = X86_MODE_32; c->cr0 = X86_CR0_PE;
    c->gdtr_base = 0x1000; c->gdtr_limit = 0x2f; c->seg_base[X86_CS] = 0;
    c->seg_limit[X86_CS] = 0xffffffffu; c->rip = 0x500;
    /* TSS at 0x234500, limit 0x67; available 32-bit TSS, present. */
    const uint8_t tss[] = {0x67,0,0x00,0x45,0x23,0x89,0,0};
    memcpy(m->memory + 0x1028, tss, sizeof(tss));
    const uint8_t code[] = {0x31,0xc0, 0x0f,0x00,0xd0, /* LLDT AX=0 */
                            0x66,0xb8,0x28,0x00, 0x0f,0x00,0xd8, /* LTR AX */
                            0x0f,0x00,0xc9}; /* STR CX */
    memcpy(m->memory + 0x500, code, sizeof(code));
    c->ldtr_sel = 0x18; c->ldtr_base = 0x1234; c->ldtr_limit = 0x100;
    CHECK(x86_step(c)); CHECK(x86_step(c));
    CHECK(c->ldtr_sel == 0 && c->ldtr_base == 0 && c->ldtr_limit == 0);
    CHECK(x86_step(c)); CHECK(x86_step(c));
    CHECK(c->tr_sel == 0x28 && c->tr_base == 0x234500 && c->tr_limit == 0x67);
    CHECK(m->memory[0x102d] == 0x8b);
    CHECK(x86_step(c)); CHECK((c->gpr[X86_RCX] & 0xffffu) == 0x28);
}

static void test_imul_immediate(PcMachine* m) {
    X86Cpu* c = &m->cpu;
    x86_reset(c, m->memory, m->memory_size);
    x86_set_max_mode(c, X86_MODE_32);
    c->wide_ready = 1; c->mode = X86_MODE_32; c->cr0 = X86_CR0_PE;
    c->seg_base[X86_CS] = 0; c->rip = 0x500;
    const uint8_t code[] = {0xb8,0xfe,0xff,0xff,0xff, /* eax=-2 */
                            0x6b,0xc0,0xfd,            /* eax *= -3 */
                            0xb8,0xff,0xff,0xff,0x7f,
                            0x69,0xc8,2,0,0,0};       /* ecx=eax*2 */
    memcpy(m->memory + 0x500, code, sizeof(code));
    CHECK(x86_step(c)); CHECK(x86_step(c));
    CHECK(c->gpr[X86_RAX] == 6 && !(c->flags & (X86_CF | X86_OF)));
    CHECK(x86_step(c)); CHECK(x86_step(c));
    CHECK(c->gpr[X86_RCX] == 0xfffffffeu &&
          (c->flags & (X86_CF | X86_OF)) == (X86_CF | X86_OF));
}

static void test_real_exception(PcMachine* m) {
    x86_reset(&m->cpu, m->memory, m->memory_size);
    x86_set_max_mode(&m->cpu, X86_MODE_32);
    m->cpu.sregs[X86_CS] = m->cpu.sregs[X86_SS] = 0;
    m->cpu.ip = 0x500; m->cpu.regs[X86_SP] = 0x800; m->cpu.flags = 0x202;
    m->memory[0x500] = 0x0f; m->memory[0x501] = 0x0b; /* UD2 */
    x86_write_word(&m->cpu, 6 * 4, 0x650);
    x86_write_word(&m->cpu, 6 * 4 + 2, 0);
    CHECK(x86_step(&m->cpu));
    CHECK(m->cpu.ip == 0x650 && m->cpu.regs[X86_SP] == 0x7fa);
    CHECK(x86_read_word(&m->cpu, 0x7fa) == 0x500);
    CHECK(x86_read_word(&m->cpu, 0x7fc) == 0);
    CHECK(x86_read_word(&m->cpu, 0x7fe) == 0x202);
}

static void test_halt_wakeup(PcMachine* m) {
    x86_reset(&m->cpu, m->memory, m->memory_size);
    devices_reset(m);
    legacy_bios_install(m);
    m->cpu.sregs[X86_CS] = 0;
    m->cpu.sregs[X86_SS] = 0;
    m->cpu.regs[X86_SP] = 0x800;
    m->cpu.ip = 0x500;
    m->cpu.flags = 2;
    m->status = OPENXISO_MACHINE_RUNNING;
    const uint8_t code[] = {0xfb, 0xf4, 0xb8, 0x01, 0x00, 0xfa, 0xf4};
    memcpy(m->memory + 0x500, code, sizeof(code));
    openxiso_machine_run_slice(m, 2);
    CHECK(m->cpu.halted && m->status == OPENXISO_MACHINE_WAITING);
    devices_port_out(m, 0x21, 0xfe); /* unmask IRQ0 */
    devices_raise_irq(m, 0);
    openxiso_machine_run_slice(m, 1);
    CHECK(!m->cpu.halted && m->cpu.regs[X86_AX] == 1);
    openxiso_machine_run_slice(m, 2);
    CHECK(m->cpu.halted && m->status == OPENXISO_MACHINE_HALTED);
}

static void test_paging(PcMachine* m) {
    X86Cpu* c = &m->cpu;
    x86_reset(c, m->memory, m->memory_size);
    c->mode = X86_MODE_32;
    c->cr0 = X86_CR0_PE | X86_CR0_PG;
    c->cr3 = 0x1000;
    memset(m->memory + 0x1000, 0, 0x3000);
    int pf = 99;
    x86_translate(c, 0x123, 0, 0, &pf);
    CHECK(pf == 0 && c->cr2 == 0x123); /* zero is a real #PF code */
    x86_write_physical(c, 0x1000, 32, 0x2003);
    x86_write_physical(c, 0x2000, 32, 0x3003);
    CHECK(x86_translate(c, 0x123, 0, 0, &pf) == 0x3123 && pf == -1);
    CHECK(x86_read_physical(c, 0x1000, 32) & 0x20);
    CHECK(x86_read_physical(c, 0x2000, 32) & 0x20);
    c->cpl = 3;
    x86_translate(c, 0x123, 0, 0, &pf); CHECK(pf == 5);
    x86_write_physical(c, 0x1000, 32, 0x2007);
    x86_write_physical(c, 0x2000, 32, 0x3005);
    x86_translate(c, 0x123, 1, 0, &pf); CHECK(pf == 7);
    c->cpl = 0;
    CHECK(x86_translate(c, 0x123, 1, 0, &pf) == 0x3123 && pf == -1);
    CHECK(x86_read_physical(c, 0x2000, 32) & 0x40);
    c->cr0 |= X86_CR0_WP;
    x86_translate(c, 0x123, 1, 0, &pf); CHECK(pf == 3);
    x86_translate(c, 0x400000, 0, 1, &pf); CHECK(pf == 16);

    /* Long-mode 1 GiB pages use bits 30+ of the physical address. */
    memset(m->memory + 0x1000, 0, 0x2000);
    c->mode = X86_MODE_64; c->efer = X86_EFER_LMA | X86_EFER_NXE;
    c->cr4 = X86_CR4_PAE; c->cr0 &= ~X86_CR0_WP;
    x86_write_physical(c, 0x1000, 64, 0x2007);
    x86_write_physical(c, 0x2000, 64, 0x40000087);
    CHECK(x86_translate(c, 0x1234567, 0, 0, &pf) == 0x41234567 && pf == -1);
    x86_write_physical(c, 0x2000, 64, 0x8000000040000087ULL);
    x86_translate(c, 0x1234, 0, 1, &pf); CHECK(pf == 17);
    x86_translate(c, 0x1234, 0, 0, &pf); CHECK(pf == -1);
}

static void test_interrupt_gate(PcMachine* m) {
    X86Cpu* c = &m->cpu;
    x86_reset(c, m->memory, m->memory_size);
    memset(m->memory + 0x5000, 0, 0x3000);
    c->mode = X86_MODE_32; c->cr0 = X86_CR0_PE; c->gdtr_base = 0x5000;
    c->gdtr_limit = 0x17; c->idtr_base = 0x6000; c->idtr_limit = 0x107;
    const uint8_t code_descriptor[8] = {0xff,0xff,0,0,0,0x9a,0xcf,0};
    memcpy(m->memory + 0x5008, code_descriptor, sizeof(code_descriptor));
    CHECK(x86_load_segment(c, X86_CS, 8) == 0);
    CHECK(c->seg_limit[X86_CS] == 0xffffffffu);
    c->seg_flags[X86_SS] = 0x40; c->seg_base[X86_SS] = 0;
    c->gpr[X86_RSP] = 0x9000; c->rip = 0x12345678; c->flags = 0x202;
    uint8_t* gate = m->memory + 0x6000 + 0x20 * 8;
    gate[0]=0x34; gate[1]=0x12; gate[2]=8; gate[5]=0x8e; gate[6]=0x78; gate[7]=0x56;
    x86_deliver_interrupt(c, 0x20, 0, 0);
    CHECK(!c->triple_fault && c->rip == 0x56781234 && c->gpr[X86_RSP] == 0x8ff4);
    CHECK(x86_read_physical(c, 0x8ff4, 32) == 0x12345678);
    CHECK(x86_read_physical(c, 0x8ff8, 32) == 8);
    CHECK(x86_read_physical(c, 0x8ffc, 32) == 0x202 && !(c->flags & X86_IF));
    gate[5]=0x8f; c->gpr[X86_RSP]=0x9000; c->rip=0x1234; c->flags=0x202;
    x86_deliver_interrupt(c, 0x20, 0x55, 1);
    CHECK(!c->triple_fault && c->gpr[X86_RSP] == 0x8ff0 && (c->flags & X86_IF));
    CHECK(x86_read_physical(c, 0x8ff0, 32) == 0x55);
    CHECK(x86_read_physical(c, 0x8ff4, 32) == 0x1234);
}

int main(void) {
    PcMachine* m = openxiso_machine_create();
    if (!m) return 1;
    x86_reset(&m->cpu, m->memory, m->memory_size);
    x86_set_max_mode(&m->cpu, X86_MODE_32);
    m->cpu.context = m;
    /* Near JZ rel16 is four bytes, whether taken or not. */
    const uint8_t code[] = {0x0f, 0x84, 0x02, 0x00, 0xb0, 0x42, 0xf4};
    memcpy(m->memory + 0x100, code, sizeof(code));
    m->cpu.ip = 0x100;
    m->cpu.sregs[X86_CS] = 0;
    m->cpu.flags = 2;
    CHECK(x86_step(&m->cpu));
    CHECK(m->cpu.ip == 0x104);
    CHECK(x86_step(&m->cpu));
    CHECK((m->cpu.regs[X86_AX] & 0xff) == 0x42);
    m->cpu.ip = 0x100;
    m->cpu.flags |= X86_ZF;
    CHECK(x86_step(&m->cpu));
    CHECK(m->cpu.ip == 0x106);

    /* CMP updates flags, never either operand, in real and wide decoding. */
    const uint8_t compare[] = {0x39, 0xd8, 0x3b, 0xc3, 0x66, 0x39, 0xd0};
    memcpy(m->memory + 0x220, compare, sizeof(compare));
    m->cpu.ip = 0x220; m->cpu.regs[X86_AX] = 0x400;
    m->cpu.regs[X86_BX] = 0x100; m->cpu.regs[X86_DX] = 0x200;
    CHECK(x86_step(&m->cpu)); CHECK(m->cpu.regs[X86_AX] == 0x400);
    CHECK(x86_step(&m->cpu)); CHECK(m->cpu.regs[X86_AX] == 0x400);
    CHECK(x86_step(&m->cpu)); CHECK(m->cpu.regs[X86_AX] == 0x400);

    /* Real-mode prefixed moffs, 32-bit INC/DEC and EFLAGS save/restore. */
    x86_reset(&m->cpu, m->memory, m->memory_size);
    x86_set_max_mode(&m->cpu, X86_MODE_32);
    m->cpu.sregs[X86_CS] = m->cpu.sregs[X86_DS] = m->cpu.sregs[X86_SS] = 0;
    m->cpu.ip = 0x300; m->cpu.regs[X86_SP] = 0x900;
    m->memory[0x400] = 0x78; m->memory[0x401] = 0x56;
    m->memory[0x402] = 0x34; m->memory[0x403] = 0x12;
    const uint8_t wide_code[] = {
        0x66, 0xa1, 0x00, 0x04, /* mov eax,[0400] */
        0x66, 0x40,             /* inc eax */
        0x66, 0x48,             /* dec eax */
        0x66, 0xa3, 0x08, 0x04, /* mov [0408],eax */
        0x66, 0x9c, 0x66, 0x9d /* pushfd; popfd */
    };
    memcpy(m->memory + 0x300, wide_code, sizeof(wide_code));
    m->cpu.flags = 0x200002u | X86_CF; /* ID and carry */
    CHECK(x86_step(&m->cpu)); CHECK(m->cpu.gpr[X86_RAX] == 0x12345678u);
    CHECK(x86_step(&m->cpu)); CHECK(m->cpu.gpr[X86_RAX] == 0x12345679u && (m->cpu.flags & X86_CF));
    CHECK(x86_step(&m->cpu)); CHECK(m->cpu.gpr[X86_RAX] == 0x12345678u && (m->cpu.flags & X86_CF));
    CHECK(x86_step(&m->cpu)); CHECK(memcmp(m->memory + 0x400, m->memory + 0x408, 4) == 0);
    CHECK(x86_step(&m->cpu)); CHECK(x86_step(&m->cpu)); CHECK(m->cpu.flags & 0x200000u);
    CHECK(m->cpu.regs[X86_SP] == 0x900);

    /* PUSHAD/POPAD and PUSH/POP FS and GS preserve register and segment values. */
    const uint8_t stack_code[] = {0x66, 0x60, 0x66, 0x61, 0x0f, 0xa0, 0x0f, 0xa9};
    memcpy(m->memory + 0x320, stack_code, sizeof(stack_code));
    m->cpu.ip = 0x320; m->cpu.regs[X86_AX] = 0x1234;
    m->cpu.sregs[X86_FS] = 0x1234; m->cpu.sregs[X86_GS] = 0;
    CHECK(x86_step(&m->cpu)); CHECK(m->cpu.regs[X86_SP] == 0x8e0);
    CHECK(x86_step(&m->cpu)); CHECK(m->cpu.regs[X86_SP] == 0x900 && m->cpu.regs[X86_AX] == 0x1234);
    CHECK(x86_step(&m->cpu)); CHECK(x86_step(&m->cpu));
    CHECK(m->cpu.sregs[X86_GS] == 0x1234 && m->cpu.regs[X86_SP] == 0x900);

    /* 8042/BIOS A20 services use the documented AL subfunctions. */
    m->cpu.regs[X86_AX] = 0x2401; legacy_bios_interrupt(m, 0x15);
    CHECK(m->cpu.a20_enabled == 1 && !(m->cpu.flags & X86_CF));
    m->cpu.regs[X86_AX] = 0x2402; legacy_bios_interrupt(m, 0x15);
    CHECK((m->cpu.regs[X86_AX] & 0xff) == 1);
    m->cpu.regs[X86_AX] = 0x2400; legacy_bios_interrupt(m, 0x15);
    CHECK(m->cpu.a20_enabled == 0);

    devices_port_out(m, 0x64, 0xdf); CHECK(m->cpu.a20_enabled == 1);
    devices_port_out(m, 0x64, 0xdd); CHECK(m->cpu.a20_enabled == 0);
    devices_port_out(m, 0x64, 0xd1); devices_port_out(m, 0x60, 3);
    CHECK(m->cpu.a20_enabled == 1);
    devices_port_out(m, 0x64, 0xd0); CHECK(devices_port_in(m, 0x60) == 3);

    /* MOV GS,BX must not accidentally change CS. */
    const uint8_t mov_gs[] = {0x8e, 0xeb};
    memcpy(m->memory + 0x200, mov_gs, sizeof(mov_gs));
    m->cpu.ip = 0x200; m->cpu.sregs[X86_CS] = 0; m->cpu.regs[X86_BX] = 0x1234;
    CHECK(x86_step(&m->cpu));
    CHECK(m->cpu.sregs[X86_CS] == 0 && m->cpu.sregs[X86_GS] == 0x1234);

    /* PCI configuration mechanism 1: vendor and device ID, byte by byte. */
    devices_port_out(m, 0xcf8, 0); devices_port_out(m, 0xcf9, 0);
    devices_port_out(m, 0xcfa, 0); devices_port_out(m, 0xcfb, 0x80);
    CHECK(devices_port_in(m, 0xcfc) == 0x86);
    CHECK(devices_port_in(m, 0xcfd) == 0x80);
    CHECK(devices_port_in(m, 0xcfe) == 0x37);
    CHECK(devices_port_in(m, 0xcff) == 0x12);
    devices_port_out(m, 0xcfc, 0);
    CHECK(devices_port_in(m, 0xcfc) == 0x86);
    devices_port_out(m, 0xcfb, 0);
    CHECK(devices_port_in(m, 0xcfc) == 0xff);

    /* Standard raw floppy geometry without requiring a FAT BPB. */
    FILE* f = tmpfile();
    CHECK(f != NULL);
    if (f) {
        uint8_t boot[512] = {0}; boot[510] = 0x55; boot[511] = 0xaa;
        CHECK(fwrite(boot, 1, sizeof(boot), f) == sizeof(boot));
        CHECK(fflush(f) == 0);
        CHECK(ftruncate(fileno(f), 1440 * 1024) == 0);
        OpenXisoBootPlan plan; char error[192];
        CHECK(openxiso_boot_plan(fileno(f), 1440 * 1024, &plan, error, sizeof(error)) == 0);
        CHECK(plan.drive == 0 && plan.sectors_per_track == 18 && plan.heads == 2);
        fclose(f);
    }
    test_eltorito_status(m);
    test_eltorito_load_segment(m);
    test_unprefixed_protected_far_jump(m);
    test_return_to_real_cs_cache(m);
    test_address32_indirect_far_call(m);
    test_ldtr_and_tr(m);
    test_imul_immediate(m);
    test_real_exception(m);
    test_halt_wakeup(m);
    test_paging(m);
    test_interrupt_gate(m);
    openxiso_machine_destroy(m);
    printf("Проверка устройств и загрузчика: ошибок %d\n", failures);
    return failures ? 1 : 0;
}
