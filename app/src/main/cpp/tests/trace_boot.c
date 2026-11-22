/* Bounded instruction trace for diagnosing BIOS boot loaders; diagnostic only. */
#include "pc_machine.h"
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct Trace {
    uint64_t n, ip;
    uint16_t cs, ds, es, ss;
    uint32_t ax, bx, cx, dx, si, di, sp;
    uint32_t flags, cr0;
    uint8_t bytes[6];
} Trace;

static Trace capture(PcMachine *m) {
    X86Cpu *c = &m->cpu;
    Trace t = {0};
    t.n = c->instructions; t.cs = c->sregs[X86_CS];
    t.ip = c->mode == X86_MODE_16 ? c->ip : c->rip;
    t.ax = c->mode == X86_MODE_16 ? c->regs[X86_AX] : (uint32_t)c->gpr[X86_RAX];
    t.bx = c->mode == X86_MODE_16 ? c->regs[X86_BX] : (uint32_t)c->gpr[X86_RBX];
    t.cx = c->mode == X86_MODE_16 ? c->regs[X86_CX] : (uint32_t)c->gpr[X86_RCX];
    t.dx = c->mode == X86_MODE_16 ? c->regs[X86_DX] : (uint32_t)c->gpr[X86_RDX];
    t.si = c->mode == X86_MODE_16 ? c->regs[X86_SI] : (uint32_t)c->gpr[X86_RSI];
    t.di = c->mode == X86_MODE_16 ? c->regs[X86_DI] : (uint32_t)c->gpr[X86_RDI];
    t.ds = c->sregs[X86_DS]; t.es = c->sregs[X86_ES]; t.ss = c->sregs[X86_SS];
    t.sp = c->mode == X86_MODE_16 ? c->regs[X86_SP] : (uint32_t)c->gpr[X86_RSP];
    t.flags = c->flags; t.cr0 = c->cr0;
    for (unsigned i = 0; i < sizeof(t.bytes); i++) {
        if (!(c->cr0 & X86_CR0_PG)) {
            uint64_t addr = (c->cr0 & X86_CR0_PE) ? c->seg_base[X86_CS] + t.ip + i :
                            x86_physical_a20(c, t.cs, (uint16_t)(t.ip + i));
            t.bytes[i] = (uint8_t)x86_read_physical(c, addr, 8);
        }
    }
    return t;
}
static char last_lines[48][256];
static volatile sig_atomic_t last_count;
static void timeout_handler(int signal_number) {
    (void)signal_number;
    const char header[] = "\nExecution timed out; most recent instructions:\n";
    (void)write(STDOUT_FILENO, header, sizeof(header) - 1);
    unsigned count = (unsigned)last_count;
    unsigned first = count > 48 ? count - 48 : 0;
    for (unsigned i = first; i < count; i++) {
        size_t n = 0;
        while (n < sizeof(last_lines[0]) && last_lines[i % 48][n]) ++n;
        (void)write(STDOUT_FILENO, last_lines[i % 48], n);
    }
    _exit(124);
}
static void format_trace(char *buf, size_t size, const Trace *t) {
    snprintf(buf, size, "%" PRIu64 " %04x:%04" PRIx64 " %02x %02x %02x %02x %02x %02x "
           "AX=%08x BX=%08x CX=%08x DX=%08x SI=%08x DI=%08x DS=%04x ES=%04x SS:SP=%04x:%08x FL=%04x CR0=%08x\n",
           t->n, t->cs, t->ip, t->bytes[0], t->bytes[1], t->bytes[2],
           t->bytes[3], t->bytes[4], t->bytes[5], t->ax, t->bx, t->cx, t->dx,
           t->si, t->di, t->ds, t->es, t->ss, t->sp, t->flags, t->cr0);
}
static void print_trace(const Trace *t) {
    char buf[256]; format_trace(buf, sizeof(buf), t); fputs(buf, stdout);
}
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s image\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { perror("image"); return 2; }
    PcMachine *m = openxiso_machine_create();
    if (!m || openxiso_machine_set_cpu_mode(m, 1) ||
        openxiso_machine_load_image(m, fd, st.st_size) || openxiso_machine_start(m)) {
        fprintf(stderr, "Cannot start image\n"); return 2;
    }
    Trace ring[48] = {{0}}; unsigned count = 0; int reported = 0; int follow = 0;
    unsigned disk_reports = 0, cr0_reports = 0, firmware_reports = 0; int ud_reported = 0;
    const char *trace_ip = getenv("OPENXISO_TRACE_IP");
    uint64_t watch_ip = trace_ip ? strtoull(trace_ip, NULL, 0) : UINT64_MAX;
    int watch_reported = 0;
    signal(SIGALRM, timeout_handler);
    alarm(20);
    for (unsigned i = 0; i < 20000000; i++) {
        Trace t = capture(m); ring[count % 48] = t;
        format_trace(last_lines[count % 48], sizeof(last_lines[0]), &t);
        ++count; last_count = (sig_atomic_t)count;
        if (!watch_reported && t.ip == watch_ip) {
            watch_reported = 1; puts("First watched instruction:");
            unsigned first = count > 32 ? count - 32 : 0;
            for (unsigned j = first; j < count; j++) print_trace(&ring[j % 48]);
        }
        if (follow > 0) { printf("Transition: "); print_trace(&t); --follow; }
        if (t.cs == 0xf000 && t.ip >= 0x1000 && t.ip < 0x2000 &&
            ((t.ip - 0x1000) % 16 == 0) && firmware_reports++ < 15) {
            printf("Firmware entry vector %02" PRIx64 ": ", (t.ip - 0x1000) / 16); print_trace(&t);
        }
        int disk = t.bytes[0] == 0xcd && t.bytes[1] == 0x13 &&
                   ((t.ax >> 8) == 0x4b || (t.ax >> 8) == 0x42);
        int report_disk = disk && disk_reports++ < 20;
        if (report_disk) { printf("BIOS before: "); print_trace(&t); }
        int running = openxiso_machine_run_slice(m, 1);
        if (report_disk) { Trace after = capture(m); printf("BIOS after:  "); print_trace(&after); }
        if (!ud_reported && (m->cpu.cr0 & X86_CR0_PE) && m->cpu.idtr_limit >= 55) {
            uint64_t gate = x86_read_physical(&m->cpu, m->cpu.idtr_base + 6 * 8, 64);
            uint64_t handler = (gate & 0xffffu) | ((gate >> 32) & 0xffff0000u);
            if ((gate & (UINT64_C(1) << 47)) && m->cpu.rip == handler &&
                m->cpu.sregs[X86_CS] == ((gate >> 16) & 0xffffu)) {
                ud_reported = 1;
                puts("First invalid-opcode exception:");
                unsigned first = count > 12 ? count - 12 : 0;
                for (unsigned j = first; j < count; j++) print_trace(&ring[j % 48]);
            }
        }
        if (t.cr0 != m->cpu.cr0 && cr0_reports++ < 4) {
            follow = 5;
            printf("CR0 change: "); print_trace(&t);
            printf("CR0 after: %08x GDTR=%" PRIx64 ":%04x IDTR=%" PRIx64 ":%04x\n",
                   m->cpu.cr0, m->cpu.gdtr_base, m->cpu.gdtr_limit,
                   m->cpu.idtr_base, m->cpu.idtr_limit);
            for (int d = 0; d < 6; d++)
                printf("GDT[%02x]=%016" PRIx64 "\n", d * 8,
                       x86_read_physical(&m->cpu, m->cpu.gdtr_base + d * 8, 64));
        }
        if (!reported && m->cpu.instructions > 5000 && m->cpu.sregs[X86_CS] == 0 &&
            m->cpu.ip >= 0x7c00 && m->cpu.ip < 0x8200 &&
            (t.cs != 0 || t.ip < 0x7c00 || t.ip >= 0x8800)) {
            reported = 1;
            puts("Jump back to boot sector:");
            unsigned first = count > 24 ? count - 24 : 0;
            for (unsigned j = first; j < count; j++) print_trace(&ring[j % 48]);
        }
        if (!running) break;
    }
    printf("Last instructions:\n");
    unsigned first = count > 48 ? count - 48 : 0;
    for (unsigned i = first; i < count; i++) print_trace(&ring[i % 48]);
    printf("Status %d: %s\n", m->status, openxiso_machine_error(m));
    openxiso_machine_destroy(m); close(fd); return 0;
}
