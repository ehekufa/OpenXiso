/* Bounded instruction trace for diagnosing BIOS boot loaders; diagnostic only. */
#include "pc_machine.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct Trace {
    uint64_t n;
    uint16_t cs, ip, ax, bx, cx, dx, si, di, ds, es, ss, sp;
    uint32_t flags;
    uint8_t bytes[6];
} Trace;

static Trace capture(PcMachine *m) {
    X86Cpu *c = &m->cpu;
    Trace t = {0};
    t.n = c->instructions; t.cs = c->sregs[X86_CS]; t.ip = c->ip;
    t.ax = c->regs[X86_AX]; t.bx = c->regs[X86_BX]; t.cx = c->regs[X86_CX];
    t.dx = c->regs[X86_DX]; t.si = c->regs[X86_SI]; t.di = c->regs[X86_DI];
    t.ds = c->sregs[X86_DS]; t.es = c->sregs[X86_ES]; t.ss = c->sregs[X86_SS];
    t.sp = c->regs[X86_SP]; t.flags = c->flags;
    for (unsigned i = 0; i < sizeof(t.bytes); i++)
        t.bytes[i] = x86_read_byte(c, x86_physical_a20(c, t.cs, (uint16_t)(t.ip + i)));
    return t;
}
static void print_trace(const Trace *t) {
    printf("%" PRIu64 " %04x:%04x %02x %02x %02x %02x %02x %02x "
           "AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x DS=%04x ES=%04x SS:SP=%04x:%04x FL=%04x\n",
           t->n, t->cs, t->ip, t->bytes[0], t->bytes[1], t->bytes[2],
           t->bytes[3], t->bytes[4], t->bytes[5], t->ax, t->bx, t->cx, t->dx,
           t->si, t->di, t->ds, t->es, t->ss, t->sp, t->flags);
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
    Trace ring[48] = {{0}}; unsigned count = 0;
    for (unsigned i = 0; i < 20000000; i++) {
        Trace t = capture(m); ring[count++ % 48] = t;
        int disk = t.bytes[0] == 0xcd && t.bytes[1] == 0x13 &&
                   ((t.ax >> 8) == 0x4b || (t.ax >> 8) == 0x42);
        if (disk) { printf("BIOS before: "); print_trace(&t); }
        int running = openxiso_machine_run_slice(m, 1);
        if (disk) { Trace after = capture(m); printf("BIOS after:  "); print_trace(&after); }
        if (!running) break;
    }
    printf("Last instructions:\n");
    unsigned first = count > 48 ? count - 48 : 0;
    for (unsigned i = first; i < count; i++) print_trace(&ring[i % 48]);
    printf("Status %d: %s\n", m->status, openxiso_machine_error(m));
    openxiso_machine_destroy(m); close(fd); return 0;
}
