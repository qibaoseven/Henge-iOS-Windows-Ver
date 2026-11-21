/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "winvm.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <os/lock.h>
#include <pthread/qos.h>
#endif
#include <TargetConditionals.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "audio_mac.h"
#include "dispatch.h"
#include "machine.h"
#include "mmu.h"
#include "net_slirp.h"
#include "share.h"

static HvMachine m;
static HvCpu cpus[HV_MAX_CPUS];
static pthread_t sec_threads[HV_MAX_CPUS];
static int ncpus = 1;
static HvWinVirt board;
static HvDisk disk0 = {.fd = -1};
static HvDisk disk1 = {.fd = -1};
static int net_on;
static int share_on;
static _Atomic int stop_requested;

static os_unfair_lock display_lock = OS_UNFAIR_LOCK_INIT;
static CFTypeRef display_layer;
static int display_layer_set;
static _Atomic(HvFbDisplay *) display_fb;

static void (*exit_cb)(int reason);

int hv_winvm_share_up(void) { return share_on; }
HvMachine *hv_winvm_machine(void) { return &m; }
HvWinVirt *hv_winvm_board(void) { return &board; }

const char *hv_winvm_reason_name(int r) {
    switch (r) {
    case HV_EXIT_HALT: return "halt";
    case HV_EXIT_WFI: return "wfi with nothing to wake it";
    case HV_EXIT_UNSUPPORTED: return "unsupported instruction";
    case HV_EXIT_FAULT_LOOP: return "fault loop on exception vector";
    default: return "other";
    }
}

/* Why the last block returned to the dispatcher, for the sampler below. */
static const char *exit_reason_name(int r) {
    switch (r) {
    case HV_EXIT_NONE: return "none";
    case HV_EXIT_BRANCH: return "branch";
    case HV_EXIT_SYSREG: return "sysreg";
    case HV_EXIT_EXCEPTION: return "exception";
    case HV_EXIT_WFI: return "wfi";
    case HV_EXIT_UNDEF: return "undef";
    case HV_EXIT_UNSUPPORTED: return "unsupported";
    case HV_EXIT_HALT: return "halt";
    case HV_EXIT_FLUSH: return "flush";
    case HV_EXIT_FAULT_LOOP: return "faultloop";
    default: return "?";
    }
}

/* ---- progress sampler: what every CPU is doing, once a second -------------
   A guest that hangs without touching a device leaves no trace at all: no
   CDBs, no key reports, no timer deliveries, nothing in any other log. The
   only way to tell "the guest is stuck at address X" from "we have no data"
   is to write down its PC, its block counter and the registers a spinlock
   would wait on, while it is busy not doing anything. `+n` after ic= is the
   blocks that CPU entered during the last second: 0 means it is not running,
   not even through the dispatcher. */
static double sample_t0;
static uint64_t sample_icount[HV_MAX_CPUS];
static uint64_t sample_blocks, sample_chained;
static int sample_stalled;

static double sample_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Read guest memory the way the guest would, from the sampler thread: the page
   tables it finds and the RAM behind them are only read. */
static int guest_read(HvCpu *c, uint64_t va, void *buf, unsigned len, HvAccess acc) {
    HvWalk w;
    hv_mmu_walk(c, va, acc, c->el, &w);
    if (w.fsc) return 0;
    return hv_phys_read(c->m, w.pa, buf, len) == 0;
}

/* A word about one instruction, enough to read a loop off the log. */
static const char *stall_note(uint32_t w, uint64_t va, char *b, size_t n) {
    int64_t o;
    if ((w & 0xfc000000u) == 0x14000000u) { /* B */
        o = (int64_t)(w & 0x03ffffffu);
        if (o & 0x02000000LL) o -= 0x04000000LL;
        snprintf(b, n, "b %llx", (unsigned long long)(va + (uint64_t)(o * 4)));
        return b;
    }
    if ((w & 0xfc000000u) == 0x94000000u) {
        o = (int64_t)(w & 0x03ffffffu);
        if (o & 0x02000000LL) o -= 0x04000000LL;
        snprintf(b, n, "bl %llx", (unsigned long long)(va + (uint64_t)(o * 4)));
        return b;
    }
    if ((w & 0xff000010u) == 0x54000000u) { /* B.cond */
        o = (int64_t)((w >> 5) & 0x7ffffu);
        if (o & 0x40000LL) o -= 0x80000LL;
        snprintf(b, n, "b.%u %llx", (unsigned)(w & 0xf), (unsigned long long)(va + (uint64_t)(o * 4)));
        return b;
    }
    if ((w & 0xfffffc1fu) == 0xd65f0000u) return "ret";
    if ((w & 0xfe9f0000u) == 0xd61f0000u) return (w & 0x001f0000u) == 0x001f0000u ? "blr xN" : "br xN";
    if ((w & 0x7e000000u) == 0x34000000u || (w & 0x7e000000u) == 0x36000000u) return "cbz/cbnz/tbz/tbnz";
    if ((w & 0xffe0001fu) == 0xd503201fu) return "nop/hint";
    return "";
}

/* ---- stall memory: files, not log lines -------------------------------------
   A hang is only worth chasing with the memory it hung in, but forty megabytes
   of hex in the log would bury every other line. The window around the
   addresses the dump prints is written to files instead; the log keeps the
   ranges, the sizes and what could not be read, so the files can be laid
   against the guest's addresses afterwards. */
#define STALL_CHUNK (1u << 20)
#define STALL_SLACK (20ull << 20)    /* this much either side of the outermost pointer */
#define STALL_STACK_UP (1ull << 20)  /* the stack: 64 KB below sp, 1 MB above */
#define STALL_STACK_DOWN (64ull << 10)
#define STALL_MAX (96ull << 20)

static int stall_ram_of(HvCpu *c, uint64_t va, uint64_t *start, uint64_t *end) {
    for (int i = 0; i < c->m->nram; i++) {
        HvRam *r = &c->m->ram[i];
        if (va >= r->pa && va - r->pa < r->size) {
            *start = r->pa;
            *end = r->pa + r->size;
            return 1;
        }
    }
    return 0;
}

/* Copy guest memory a page at a time: the window may span blocks of more than
   one size, and a page that cannot be read is written as zeros and counted.
   Reading it through the current address space is only half the job: page
   tables themselves are physical memory that the address space in force may
   have no mapping for, and they are exactly what a fault has to be read
   against. So a page the walk cannot reach is read as physical memory (identity)
   instead, and counted separately. */
static void stall_copy(HvCpu *c, uint64_t va, uint8_t *buf, unsigned long len,
                       unsigned long *bad, unsigned long *phys) {
    unsigned long done = 0;
    while (done < len) {
        unsigned long n = 4096ul - (unsigned long)((va + done) & 4095ul);
        HvWalk w;
        if (n > len - done) n = len - done;
        hv_mmu_walk(c, va + done, HV_ACC_READ, c->el, &w);
        if (w.fsc) hv_mmu_walk(c, va + done, HV_ACC_EXEC, c->el, &w); /* code pages are often read-protected */
        if (!w.fsc && hv_phys_read(c->m, w.pa, buf + done, (unsigned)n) == 0) {
            done += n;
            continue;
        }
        if (hv_phys_read(c->m, va + done, buf + done, (unsigned)n) == 0) {
            if (phys) (*phys)++;
        } else {
            memset(buf + done, 0, n);
            if (bad) (*bad)++;
        }
        done += n;
    }
}

static char *stall_path(const char *what, uint64_t pc, char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Documents/stall-%s-%llx.bin", home ? home : "/tmp", what, (unsigned long long)pc);
    return out;
}

static unsigned long long stall_write(HvCpu *c, FILE *f, uint64_t from, uint64_t len, unsigned long *bad,
                                      unsigned long *phys) {
    uint8_t *buf = malloc(STALL_CHUNK);
    unsigned long long written = 0;
    if (!buf) return 0;
    for (uint64_t off = 0; off < len; off += STALL_CHUNK) {
        unsigned long n = (unsigned long)(len - off < STALL_CHUNK ? len - off : STALL_CHUNK);
        stall_copy(c, from + off, buf, n, bad, phys);
        fwrite(buf, 1, n, f);
        written += n;
    }
    free(buf);
    return written;
}

/* The guest has not left one PC for five seconds: say where it is, what the
   code there is, and what the addresses it holds point at. */
static void stall_dump(HvCpu *c) {
    uint64_t pc = c->pc;
    c->stall_reported++;
    fprintf(stderr, "\nwinvm: *** cpu %u has not left pc %llx el%u for %u s (ic %llu): the guest is waiting, not booting ***\n",
            c->index, (unsigned long long)pc, c->el, (unsigned)c->stall_n, (unsigned long long)c->icount);
    fprintf(stderr, "  daif %llx sctlr %llx vbar %llx ttbr0 %llx ttbr1 %llx tcr %llx\n",
            (unsigned long long)c->daif, (unsigned long long)c->sctlr_el1, (unsigned long long)c->vbar_el1,
            (unsigned long long)c->ttbr0_el1, (unsigned long long)c->ttbr1_el1, (unsigned long long)c->tcr_el1);
    fprintf(stderr, "  esr %llx (ec %#llx iss %#llx) far %llx elr %llx spsr %llx sp %llx\n",
            (unsigned long long)c->esr_el1, (unsigned long long)(c->esr_el1 >> 26),
            (unsigned long long)(c->esr_el1 & 0x1ffffff), (unsigned long long)c->far_el1,
            (unsigned long long)c->elr_el1, (unsigned long long)c->spsr_el1, (unsigned long long)c->sp);
    for (int r = 0; r < 30; r += 4)
        fprintf(stderr, "  x%d %llx x%d %llx x%d %llx x%d %llx\n", r, (unsigned long long)c->x[r],
                r + 1, (unsigned long long)c->x[r + 1], r + 2, (unsigned long long)c->x[r + 2],
                r + 3, (unsigned long long)c->x[r + 3]);
    fprintf(stderr, "  x29 %llx x30 %llx\n", (unsigned long long)c->x[29], (unsigned long long)c->x[30]);
    {
        uint64_t bases[2] = {pc, c->elr_el1};
        for (int b = 0; b < 2; b++) {
            char note[48];
            uint64_t base = bases[b] & ~0xfull;
            if (b && (bases[b] < pc - 0x2000 || bases[b] > pc + 0x2000)) {
                fprintf(stderr, "  -- around elr --\n");
            } else if (b) {
                continue;
            }
            for (int i = 0; i < 16; i++) {
                uint64_t va = base + 4 * i;
                uint32_t w = 0;
                int ok = guest_read(c, va, &w, 4, HV_ACC_EXEC);
                const char *sep = i % 4 == 3 ? "\n" : "  ";
                if (i % 4 == 0) fprintf(stderr, "  ");
                note[0] = 0;
                if (ok) fprintf(stderr, "%llx: %08x %-14s%s", (unsigned long long)va, w,
                               stall_note(w, va, note, sizeof note), sep);
                else fprintf(stderr, "%llx: (unreadable)%s", (unsigned long long)va, sep);
            }
        }
    }
    { /* the vector table the sync entry came from */
        uint64_t vb = c->vbar_el1 & ~0x7ffull;
        uint64_t w = 0;
        if (vb && guest_read(c, vb + 0x200, &w, 8, HV_ACC_EXEC))
            fprintf(stderr, "  vector %llx+0x200 = %llx %08x\n", (unsigned long long)vb,
                    (unsigned long long)w, (unsigned)w);
    }
    { /* what it is polling: the words the registers point at */
        for (int r = 0; r < 8; r++) {
            uint64_t b = c->x[r], w0 = 0, w8 = 0, w16 = 0, w64 = 0;
            if (!b) continue;
            int o0 = guest_read(c, b, &w0, 8, HV_ACC_READ), o8 = guest_read(c, b + 8, &w8, 8, HV_ACC_READ);
            int o16 = guest_read(c, b + 16, &w16, 8, HV_ACC_READ), o64 = guest_read(c, b + 64, &w64, 8, HV_ACC_READ);
            if (!o0 && !o8 && !o16 && !o64) continue;
            fprintf(stderr, "  [x%d] %llx: %llx %llx %llx ... %llx\n", r, (unsigned long long)b,
                    o0 ? (unsigned long long)w0 : 0ull, o8 ? (unsigned long long)w8 : 0ull,
                    o16 ? (unsigned long long)w16 : 0ull, o64 ? (unsigned long long)w64 : 0ull);
        }
    }
    { /* the frame-pointer chain, as far as it goes */
        uint64_t fp = c->x[29];
        fprintf(stderr, "  call stack:");
        for (int i = 0; i < 32 && fp; i++) {
            uint64_t next = 0, ret = 0;
            if (!guest_read(c, fp, &next, 8, HV_ACC_READ) || !guest_read(c, fp + 8, &ret, 8, HV_ACC_READ)) break;
            fprintf(stderr, "%s#%d fp %llx ret %llx", i ? "" : "\n", i, (unsigned long long)fp,
                    (unsigned long long)ret);
            if (i % 2) fprintf(stderr, "\n");
            if (next <= fp || next - fp > (1ull << 24)) break;
            fp = next;
        }
        fprintf(stderr, "\n");
    }
    if (c->stall_reported == 1) { /* the memory itself, once: files, not log lines */
        uint64_t start = 0, end = 0;
        int in_ram = stall_ram_of(c, pc, &start, &end);
        {
            /* pc +/- STALL_SLACK: the code, the image headers above it and the
               caller pages below it all sit inside that. A kernel PC is a
               virtual address outside RAM's range entirely, so the window is
               virtual there and pages that are not mapped come out as zeros. */
            uint64_t lo, hi;
            if (in_ram) {
                lo = pc > start + STALL_SLACK ? pc - STALL_SLACK : start;
                hi = pc + STALL_SLACK < end ? pc + STALL_SLACK : end;
            } else {
                lo = pc > STALL_SLACK ? pc - STALL_SLACK : 0;
                hi = pc + STALL_SLACK;
            }
            if (hi > lo + STALL_MAX) hi = lo + STALL_MAX;
            char path[256];
            unsigned long bad = 0, phys = 0;
            FILE *f = fopen(stall_path("mem", pc, path, sizeof path), "wb");
            unsigned long long n = 0;
            if (f) {
                n = stall_write(c, f, lo, hi - lo, &bad, &phys);
                fclose(f);
            }
            fprintf(stderr, "  memory %llx..%llx (%llu KB, %lu unreadable pages, %lu read as physical) -> %s\n",
                    (unsigned long long)lo, (unsigned long long)hi, n >> 10, bad, phys, f ? path : "(open failed)");
            /* the stack, read once: written out, and searched for call sites */
            uint64_t slo, shi;
            if (in_ram) {
                slo = c->sp > STALL_STACK_DOWN ? c->sp - STALL_STACK_DOWN : start;
                shi = c->sp + STALL_STACK_UP < end ? c->sp + STALL_STACK_UP : end;
                if (slo < start) slo = start;
            } else {
                slo = c->sp > STALL_STACK_DOWN ? c->sp - STALL_STACK_DOWN : 0;
                shi = c->sp + STALL_STACK_UP;
            }
            if (shi > slo) {
                unsigned long n2 = (unsigned long)(shi - slo), bad2 = 0, phys2 = 0;
                uint8_t *sb = malloc(n2);
                if (sb) {
                    FILE *g;
                    stall_copy(c, slo, sb, n2, &bad2, &phys2);
                    g = fopen(stall_path("stack", pc, path, sizeof path), "wb");
                    if (g) {
                        fwrite(sb, 1, n2, g);
                        fclose(g);
                    }
                    fprintf(stderr, "  stack %llx..%llx (%lu KB, %lu unreadable pages, %lu physical) -> %s\n",
                            (unsigned long long)slo, (unsigned long long)shi, n2 >> 10, bad2, phys2,
                            g ? path : "(open failed)");
                    /* anything on the stack that points into the code window is a
                       return address or a saved argument: the call sites, without
                       trusting the frame pointers. */
                    int shown = 0;
                    for (unsigned long o = 0; o + 8 <= n2 && shown < 48; o += 8) {
                        uint64_t v;
                        memcpy(&v, sb + o, 8);
                        if (v >= lo && v < hi && (v & 3) == 0) {
                            fprintf(stderr, "    stack+%lx: %llx\n", o, (unsigned long long)v);
                            shown++;
                        }
                    }
                    free(sb);
                }
            }
        }
    }
}

static void sample_cpu_pcs(void) {
    double now = sample_now();
    if (sample_t0 == 0.0) sample_t0 = now;
    unsigned long long d = (unsigned long long)((m.stats.blocks - sample_blocks) +
                                               (m.stats.chained - sample_chained));
    int stalled = d == 0;
    if (stalled && !sample_stalled)
        fprintf(stderr, "winvm: [pc] %.1fs *** no block entered on any CPU in 1 s: the guest is not executing ***\n",
                now - sample_t0);
    if (!stalled && sample_stalled)
        fprintf(stderr, "winvm: [pc] %.1fs *** executing again ***\n", now - sample_t0);
    sample_stalled = stalled;
    for (int i = 0; i < ncpus; i++) {
        HvCpu *c = &cpus[i];
        unsigned long long di = (unsigned long long)(c->icount - sample_icount[i]);
        sample_icount[i] = c->icount;
        fprintf(stderr, "winvm: [pc] %.1fs c%d pc=%llx el=%u ic=%llu(+%llu) last=%s/%u halt=%d"
                        " daif=%llx x0=%llx x1=%llx sp=%llx\n",
                now - sample_t0, i, (unsigned long long)c->pc, c->el, (unsigned long long)c->icount, di,
                exit_reason_name((int)c->exit_reason), (unsigned)c->exit_reason, (int)c->halted,
                (unsigned long long)c->daif, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                (unsigned long long)c->sp);
        /* Five samples with the same PC is a wait, not a boot: dump what it
           waits on while it is still standing there. WFI is a real idle. */
        if (c->pc == c->stall_pc) {
            if (++c->stall_n >= 5 && c->stall_reported < 4 && c->exit_reason != HV_EXIT_WFI && !c->halted)
                stall_dump(c);
        } else {
            c->stall_pc = c->pc;
            c->stall_n = 0;
            c->stall_reported = 0;
        }
    }
    /* Every second, not when a counter happens to land on a round number:
       a stall advances 400 blocks/s, and a sample that has to hit a multiple
       of 1024 never comes. The +n after each counter is this second's work,
       which is the only way to tell "nothing happened" from "nothing counted". */
    {
        static uint64_t b0, c0, t0s, f0, s0, p0, ff0;
        fprintf(stderr, "winvm: [stats] %.1fs blocks=%llu(+%llu) chained=%llu(+%llu) tr=%llu(+%llu)"
                        " flush_full=%llu(+%llu) syncs=%llu(+%llu) protects=%llu(+%llu) fr_fail=%llu(+%llu)\n",
                now - sample_t0, (unsigned long long)m.stats.blocks, (unsigned long long)(m.stats.blocks - b0),
                (unsigned long long)m.stats.chained, (unsigned long long)(m.stats.chained - c0),
                (unsigned long long)m.stats.translations, (unsigned long long)(m.stats.translations - t0s),
                (unsigned long long)m.stats.flush_full, (unsigned long long)(m.stats.flush_full - f0),
                (unsigned long long)m.stats.syncs, (unsigned long long)(m.stats.syncs - s0),
                (unsigned long long)m.stats.code_protects, (unsigned long long)(m.stats.code_protects - p0),
                (unsigned long long)m.stats.fr_fail[0], (unsigned long long)(m.stats.fr_fail[0] - ff0));
        b0 = m.stats.blocks;
        c0 = m.stats.chained;
        t0s = m.stats.translations;
        f0 = m.stats.flush_full;
        s0 = m.stats.syncs;
        p0 = m.stats.code_protects;
        ff0 = m.stats.fr_fail[0];
    }
    sample_blocks = m.stats.blocks;
    sample_chained = m.stats.chained;
}

static void board_poll(void) {
    if (net_on) hv_net_poll();
}

static int fail(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
    return -1;
}

/* The PC a signal landed on. Darwin nests it under uc_mcontext->__ss.__pc;
   Linux keeps it directly in uc_mcontext.pc. */
#ifdef __APPLE__
#define UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#else
#define UC_PC(uc) ((uint64_t)(uc)->uc_mcontext.pc)
#endif

static struct sigaction prior_ill, prior_bus, prior_segv;
static void on_ill(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = ctx;
    uint64_t pc = UC_PC(uc);
    uint64_t rx = (uint64_t)(uintptr_t)m.jit.base;
    uint64_t size = (uint64_t)m.jit.capacity * 4;
    int in_rx = pc >= rx && pc < rx + size;
    if (sig == SIGILL || in_rx) {
        fprintf(stderr, "winvm: signal %d in JIT: host_pc=%llx (rx=%llx +%lld) addr=%p\n",
                sig, (unsigned long long)pc, (unsigned long long)rx, (long long)(pc - rx), info->si_addr);
        for (int i = 0; i < ncpus; i++) {
            fprintf(stderr, "  cpu %d: guest_pc=%llx el=%u sctlr=%llx\n",
                    i, (unsigned long long)cpus[i].pc, cpus[i].el, (unsigned long long)cpus[i].sctlr_el1);
        }
        fflush(stderr);
    }
    struct sigaction *prior = sig == SIGILL ? &prior_ill : sig == SIGBUS ? &prior_bus : &prior_segv;
    sigaction(sig, prior, NULL);
}

static void prepare_cpu_thread(void) {
    static __thread int done;
    if (done) return;
    done = 1;
    stack_t ss = {.ss_sp = malloc(1u << 16), .ss_size = 1u << 16, .ss_flags = 0};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV);
    sigdelset(&set, SIGBUS);
    sigdelset(&set, SIGILL);
    sigdelset(&set, SIGTRAP);
    sigdelset(&set, SIGFPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    const char *q = getenv("HV_QOS");
#if TARGET_OS_IPHONE
    /* Schedule guest vCPUs on P-cores while staying strictly below UI (USER_INTERACTIVE) */
    qos_class_t c = QOS_CLASS_USER_INITIATED;
#else
    qos_class_t c = QOS_CLASS_USER_INTERACTIVE;
#endif
    if (q && !strcmp(q, "ui")) c = QOS_CLASS_USER_INTERACTIVE;
    else if (q && !strcmp(q, "init")) c = QOS_CLASS_USER_INITIATED;
    else if (q && !strcmp(q, "default")) c = QOS_CLASS_DEFAULT;
    if (c != QOS_CLASS_DEFAULT) pthread_set_qos_class_self_np(c, 0);
}

void hv_winvm_set_display_layer(void *layer) {
    CFTypeRef retained = layer ? CFRetain(layer) : NULL;
    os_unfair_lock_lock(&display_lock);
    CFTypeRef previous = display_layer;
    display_layer = retained;
    display_layer_set = 1;
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    if (fb) hv_fb_metal_set_layer(fb, layer);
    os_unfair_lock_unlock(&display_lock);
    if (previous) CFRelease(previous);
}

int hv_winvm_display_size(uint32_t *w, uint32_t *h) {
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    if (!fb) return -1;
    const uint8_t *px = NULL;
    uint32_t fw = 0, fh = 0, st = 0;
    if (hv_fb_display_get_frame(fb, &px, &fw, &fh, &st) != 0) return -1;
    if (w) *w = fw;
    if (h) *h = fh;
    return 0;
}

uint64_t hv_winvm_display_frames(void) {
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    return fb ? hv_fb_metal_frames(fb) : 0;
}

int hv_winvm_save_png(const char *path) {
    HvFbDisplay *fb = atomic_load_explicit(&display_fb, memory_order_acquire);
    return fb ? hv_fb_save_png(fb, path) : -1;
}

int hv_winvm_create(const HvWinVmConfig *c, char *err, size_t errlen) {
    if (!c || !c->code_fd || !c->vars_fd)
        return fail(err, errlen, "UEFI firmware (code.fd) and NVRAM (vars.fd) are required.");

    atomic_store(&stop_requested, 0);

    struct sigaction ill = {.sa_flags = SA_SIGINFO | SA_ONSTACK};
    ill.sa_sigaction = on_ill;
    sigaction(SIGILL, &ill, &prior_ill);
    sigaction(SIGBUS, &ill, &prior_bus);
    sigaction(SIGSEGV, &ill, &prior_segv);
    signal(SIGPIPE, SIG_IGN);

    HvDisk *d0_ptr = NULL, *d1_ptr = NULL;
    if (c->disk0 && *c->disk0) {
        if (hv_disk_open(&disk0, c->disk0, 0, 0) != 0)
            return fail(err, errlen, "Cannot open primary NVMe disk image.");
        d0_ptr = &disk0;
    }
    if (c->disk1 && *c->disk1) {
        if (hv_disk_open(&disk1, c->disk1, 0, 1) != 0)
            return fail(err, errlen, "Cannot open secondary/installer disk image.");
        d1_ptr = &disk1;
    }

    size_t jit = c->jit_bytes ? c->jit_bytes : ((size_t)768 << 20);
    int r = c->jit_rx ? hv_machine_init_jit(&m, c->jit_rx, c->jit_rw, jit) : hv_machine_init(&m, jit);
    if (r != 0) return fail(err, errlen, "Cannot initialize ARM64 JIT region (is JIT active?).");

#if TARGET_OS_IPHONE
    /* Defer synchronous guest NVMe FLUSH fsyncs on iOS APFS NAND; explicit host checkpoints call hv_disk_sync(). */
    if (!getenv("HV_FLUSH")) hv_knob[HV_K_FLUSH] = 1;
#endif

    m.log = c->log;
    m.stats_interval = c->stats_interval;
    if (getenv("HV_SLOW_MEM")) m.slow_mem = 1;
    /* TEMPORARY (winload mapping hunt): force every load and store through
       hv_mem_emulate so the PTE-write ring in core/machine.c sees the writes
       the JIT would otherwise make straight to host memory. Slower boot; drop
       this line once the missing mapping is explained. */
    m.slow_mem = 1;
    ncpus = c->ncpus < 1 ? 1 : c->ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : c->ncpus;
    m.big_lock_on = ncpus > 1 && getenv("HV_BIG_LOCK") != NULL;

    uint64_t ram_bytes = (c->ram_mb ? c->ram_mb : 4096ULL) << 20;
    static const uint8_t mac[6] = {0x02, 0x57, 0x49, 0x4e, 0x00, 0x01};
    if (hv_winvirt_create(&board, &m, cpus, ncpus, ram_bytes, c->prealloc_ram, c->code_fd, c->vars_fd,
                          d0_ptr, d1_ptr, mac, c->enable_sound, c->display_kind, c->panel_w, c->panel_h) != 0) {
        return fail(err, errlen, "Cannot create virtual ARM64 Windows board (insufficient RAM or missing firmware).");
    }
    /* ISO media: xHCI port 3 is a read-only CD-ROM. Attached here, before the CPU
       starts, so the firmware's enumeration already sees the trays closed. */
    if (c->cd0 && *c->cd0) {
        if (hv_winvirt_attach_cd(&board, c->cd0) != 0)
            return fail(err, errlen, "Cannot open the first CD-ROM image.");
    }
    if (c->cd1 && *c->cd1) {
        if (hv_winvirt_attach_cd(&board, c->cd1) != 0)
            return fail(err, errlen, "Cannot open the second CD-ROM image.");
    }
    if (c->cd2 && *c->cd2) {
        if (hv_winvirt_attach_cd(&board, c->cd2) != 0)
            return fail(err, errlen, "Cannot open the third CD-ROM image.");
    }
    /* 3D has to be decided before the first instruction runs: the guest reads the
       feature bits during reset. A failure here is not fatal -- the device just
       stays the 2D one it was. */
    if (c->gpu3d && hv_winvirt_enable_gpu3d(&board) != 0)
        fprintf(stderr, "winvm: continuing without 3D\n");

    hv_winvirt_reset_cpu(&board);

    share_on = 0;
    if (c->share_dir && *c->share_dir && c->share_sock && *c->share_sock) {
        if (hv_share_start(c->share_dir, c->share_sock, NULL) == 0) {
            hv_net_set_share_socket(c->share_sock);
            share_on = 1;
        }
    }
    if (board.net && !c->no_net) {
        if (hv_net_attach(&m, board.net) == 0) net_on = 1;
        else fprintf(stderr, "winvm: user-mode SLIRP networking unavailable\n");
    }
    if (board.hda && c->enable_sound) hv_audio_attach_hda(board.hda);
    board.poll = board_poll;

    os_unfair_lock_lock(&display_lock);
    if (display_layer_set && board.fb) hv_fb_metal_set_layer(board.fb, (void *)display_layer);
    atomic_store_explicit(&display_fb, board.fb, memory_order_release);
    os_unfair_lock_unlock(&display_lock);

    return 0;
}

static void *kicker_thread(void *unused) {
    (void)unused;
    unsigned tick = 0;
    while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
#if TARGET_OS_IPHONE
        usleep(2500);
        if (++tick >= 400) { /* 2.5 ms x 400 = one second */
            tick = 0;
            sample_cpu_pcs();
        }
#else
        usleep(1000);
        if (++tick >= 1000) { /* 1 ms x 1000 = one second */
            tick = 0;
            sample_cpu_pcs();
        }
#endif
        hv_machine_kick_all(&m);
    }
    return NULL;
}

static void *secondary_thread(void *arg) {
    HvCpu *c = arg;
    prepare_cpu_thread();
    int r = hv_run(c);
    if (!c->halted) {
        fprintf(stderr, "winvm: cpu %u stopped (%s) at pc=%llx el=%u\n",
                c->index, hv_winvm_reason_name(r), (unsigned long long)c->pc, c->el);
    }
    return NULL;
}

static void spawn_secondaries(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    for (int i = 1; i < ncpus; i++) {
        pthread_create(&sec_threads[i], &attr, secondary_thread, &cpus[i]);
    }
    pthread_attr_destroy(&attr);
}

static void join_secondaries(void) {
    for (int i = 1; i < ncpus; i++) {
        cpus[i].halted = 1;
        hv_cpu_wake(&cpus[i]);
    }
    hv_machine_kick_all(&m);
    for (int i = 1; i < ncpus; i++) {
        if (sec_threads[i]) {
            pthread_join(sec_threads[i], NULL);
            sec_threads[i] = 0;
        }
    }
}

void hv_winvm_flush_disks(void) {
    if (disk0.fd >= 0) hv_disk_sync(&disk0);
}

int hv_winvm_run_cpu0(void) {
    prepare_cpu_thread();
    for (;;) {
        int r = hv_run(&cpus[0]);
        hv_winvm_flush_disks();
        /* PSCI SYSTEM_RESET sets halted == 2: perform warm reboot without tearing down RAM/JIT! */
        if (cpus[0].halted == 2 && !atomic_load(&stop_requested)) {
            fprintf(stderr, "winvm: guest requested PSCI SYSTEM_RESET - rebooting...\n");
            join_secondaries();
            /* Reset GIC, ITS, PCIe, and pflash state for warm reboot */
            if (board.gic) {
                pthread_mutex_lock(&board.gic->mu);
                board.gic->ctlr = 0;
                memset(board.gic->enabled, 0, sizeof board.gic->enabled);
                memset(board.gic->latch, 0, sizeof board.gic->latch);
                memset(board.gic->level, 0, sizeof board.gic->level);
                memset(board.gic->active, 0, sizeof board.gic->active);
                memset(board.gic->spi_on, 0, sizeof board.gic->spi_on);
                for (int c = 0; c < HV_MAX_CPUS; c++) {
                    memset(&board.gic->cpu[c], 0, sizeof board.gic->cpu[c]);
                    board.gic->cpu[c].waker = 6;
                    for (int q = 0; q < 16; q++) board.gic->cpu[c].edge[q] = 1;
                    m.percpu[c].irq_line = 0;
                    m.percpu[c].fiq_line = 0;
                }
                pthread_mutex_unlock(&board.gic->mu);
            }
            hv_gicv3_its_reset(board.its);
            hv_pcie_reset(board.pcie);
            if (board.flash1) {
                board.flash1->cmd = 0xff;
                board.flash1->wcycle = 0;
                board.flash1->status = 0x80;
            }
            if (board.fb) board.fb->active = 0;
            m.flush_pending = 1;
            spawn_secondaries();
            continue;
        }
        /* cpu 0 is the only one that never reported where it stopped: say it. */
        if (!atomic_load(&stop_requested) && !cpus[0].halted)
            fprintf(stderr, "winvm: cpu 0 stopped (%s) at pc=%llx el=%u last=%s/%u\n",
                    hv_winvm_reason_name(r), (unsigned long long)cpus[0].pc, cpus[0].el,
                    exit_reason_name((int)cpus[0].exit_reason), (unsigned)cpus[0].exit_reason);
        return r;
    }
}

static void *cpu0_thread(void *unused) {
    (void)unused;
    int r = hv_winvm_run_cpu0();
    atomic_store(&stop_requested, 1);
    join_secondaries();
    if (exit_cb) exit_cb(r);
    return NULL;
}

void hv_winvm_start(int spawn_cpu0, void (*on_exit)(int reason)) {
    exit_cb = on_exit;
    spawn_secondaries();
    pthread_t kt;
    pthread_create(&kt, NULL, kicker_thread, NULL);
    pthread_detach(kt);
    if (spawn_cpu0) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        pthread_t ct;
        pthread_create(&ct, &attr, cpu0_thread, NULL);
        pthread_detach(ct);
        pthread_attr_destroy(&attr);
    }
}

void hv_winvm_stop(void) {
    atomic_store(&stop_requested, 1);
    hv_audio_shutdown();
    for (int i = 0; i < ncpus; i++) cpus[i].halted = 3;
    hv_machine_kick_all(&m);
    for (int i = 0; i < ncpus; i++) hv_cpu_wake(&cpus[i]);
    hv_winvm_flush_disks();
}

void hv_winvm_power_button(void) {
    hv_winvirt_power_button(&board);
}

void hv_winvm_key_hid(uint8_t usage, int down) {
    if (board.usb) hv_xhci_key(board.usb, usage, down);
}

void hv_winvm_key_linux(uint16_t code, int down) {
    if (board.usb) hv_xhci_key_linux(board.usb, code, down);
}

void hv_winvm_tablet(double x_norm, double y_norm, uint8_t buttons, int8_t wheel) {
    if (!board.usb) return;
    if (x_norm < 0.0) x_norm = 0.0;
    if (x_norm > 1.0) x_norm = 1.0;
    if (y_norm < 0.0) y_norm = 0.0;
    if (y_norm > 1.0) y_norm = 1.0;
    int ax = (int)(x_norm * 32767.0 + 0.5);
    int ay = (int)(y_norm * 32767.0 + 0.5);
    hv_xhci_tablet(board.usb, ax, ay, buttons, wheel);
}

void hv_winvm_send_ctrl_alt_del(void) {
    if (!board.usb) return;
    /* Left Ctrl (0xe0), Left Alt (0xe2), Delete (0x4c) */
    hv_xhci_key(board.usb, 0xe0, 1);
    hv_xhci_key(board.usb, 0xe2, 1);
    hv_xhci_key(board.usb, 0x4c, 1);
    hv_xhci_key(board.usb, 0x4c, 0);
    hv_xhci_key(board.usb, 0xe2, 0);
    hv_xhci_key(board.usb, 0xe0, 0);
}

void hv_winvm_usb_stats(int *kbd_ready, int *tab_ready, uint64_t *delivered) {
    HvXhci *x = board.usb;
    if (!x) {
        if (kbd_ready) *kbd_ready = 0;
        if (tab_ready) *tab_ready = 0;
        if (delivered) *delivered = 0;
        return;
    }
    pthread_mutex_lock(&x->mu);
    int ks = x->kbd_slot;
    int ts = x->tab_slot;
    if (kbd_ready) *kbd_ready = (ks >= 1 && ks <= HV_XHCI_SLOTS && x->slots[ks].ep_running[3]) ? 1 : 0;
    if (tab_ready) *tab_ready = (ts >= 1 && ts <= HV_XHCI_SLOTS && x->slots[ts].ep_running[3]) ? 1 : 0;
    if (delivered) *delivered = x->delivered;
    pthread_mutex_unlock(&x->mu);
}

void hv_winvm_engine_stats(uint64_t *jit_used_bytes, uint64_t *jit_cap_bytes,
                           uint64_t *jit_full_flushes, uint64_t *wfi_ticks) {
    if (jit_used_bytes) *jit_used_bytes = (uint64_t)m.jit.used * 4ull;
    if (jit_cap_bytes) *jit_cap_bytes = (uint64_t)m.jit.capacity * 4ull;
    if (jit_full_flushes) *jit_full_flushes = m.stats.flush_full;
    if (wfi_ticks) *wfi_ticks = m.stats.wfi_ticks;
}
