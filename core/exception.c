#include "exception.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "dispatch.h"
#include "mmu.h"

/* Only EL0 and EL1 exist for the guest; HengeVM plays EL2 (PSCI over HVC, as
   QEMU's vmapple machine sets up) and there is no EL3. */

#define PSTATE_EXTRA_MASK ((1ull << 22) | (1ull << 23) | (1ull << 12) | (1ull << 24) | (1ull << 25))

uint64_t hv_pstate_get(HvCpu *cpu) {
    return (cpu->nzcv & 0xf0000000) | (cpu->daif & HV_PSTATE_DAIF) | (cpu->pstate_extra & PSTATE_EXTRA_MASK) |
           (uint64_t)cpu->el << 2 | (cpu->el ? cpu->spsel : 0);
}

void hv_sp_switch(HvCpu *cpu, uint32_t el, uint32_t spsel) {
    if (cpu->el == 0 || cpu->spsel == 0) cpu->sp_el0 = cpu->sp;
    else cpu->sp_el1 = cpu->sp;
    cpu->el = el;
    cpu->spsel = el ? spsel : 0;
    cpu->sp = cpu->spsel ? cpu->sp_el1 : cpu->sp_el0;
}

static void set_extra(HvCpu *cpu, uint64_t extra) {
    /* PAN selects a different TLB table (hv_tlb_table), so nothing to flush. */
    cpu->pstate_extra = extra;
}

void hv_pstate_set(HvCpu *cpu, uint64_t spsr) {
    uint32_t m = spsr & 0x1f;
    uint32_t el = (m >> 2) & 3;
    if ((m & 0x10) || el > 1) el = 1; /* AArch32 and EL2/EL3 do not exist: an illegal return, kept at EL1 */
    cpu->nzcv = spsr & 0xf0000000;
    cpu->daif = spsr & HV_PSTATE_DAIF;
    set_extra(cpu, spsr & PSTATE_EXTRA_MASK);
    hv_sp_switch(cpu, el, m & 1);
}

void hv_take_exception(HvCpu *cpu, int type, uint32_t esr, uint64_t far, int set_far, uint64_t ret) {
    if (type == HV_EXC_SYNC) cpu->m->stats.exceptions++;
    else cpu->m->stats.interrupts++;
    uint64_t spsr = hv_pstate_get(cpu);
    {
        HvStats *st = &cpu->m->stats;
        uint32_t k = st->nelchg++ & 15;
        st->elchg[k].from = ret, st->elchg[k].spsr = spsr, st->elchg[k].kind = (uint32_t)type, st->elchg[k].el = cpu->el;
        st->elchg[k].to = esr;
    }
    if (cpu->m->trace || cpu->m->log >= 2)
        fprintf(stderr, "exception type %d esr %08x far %llx from pc %llx el%u\n", type, esr, (unsigned long long)far,
                (unsigned long long)ret, cpu->el);
    uint64_t offset = cpu->el == 0 ? 0x400 : cpu->spsel ? 0x200 : 0x000;
    cpu->spsr_el1 = spsr;
    cpu->elr_el1 = ret;
    /* HV_TRACE_EXC: every exception the guest takes, with the state that says
       why. A firmware that prints "Synchronous Exception" and then hangs has
       taken exactly one of these, and this is where it happened. */
    {
        static int trace = -1;
        if (trace < 0) trace = getenv("HV_TRACE_EXC") != NULL;
        if (trace)
            fprintf(stderr, "hengevm: exception type %d ec %#x pc %llx far %llx el%u sctlr %llx tcr %llx\n",
                    type, esr >> 26, (unsigned long long)cpu->pc, (unsigned long long)far, cpu->el,
                    (unsigned long long)cpu->sctlr_el1, (unsigned long long)cpu->tcr_el1);
    }
    if (type == HV_EXC_SYNC || type == HV_EXC_SERROR) cpu->esr_el1 = esr;
    if (set_far) cpu->far_el1 = far;
    hv_sp_switch(cpu, 1, 1);
    cpu->daif = HV_PSTATE_DAIF;
    uint64_t extra = cpu->pstate_extra;
    if (!((cpu->sctlr_el1 >> 23) & 1)) extra |= HV_PSTATE_PAN; /* SCTLR_EL1.SPAN == 0 */
    extra &= ~(1ull << 12);
    if ((cpu->sctlr_el1 >> 44) & 1) extra |= 1ull << 12;      /* SSBS from DSSBS */
    set_extra(cpu, extra);
    cpu->excl_addr = ~0ull;
    cpu->pc = (cpu->vbar_el1 & ~0x7ffull) + offset + (uint64_t)type * 0x80;
    cpu->exit_reason = HV_EXIT_EXCEPTION;
}

void hv_raise_sync(HvCpu *cpu, uint32_t ec, uint32_t iss, uint64_t far, uint64_t pc) {
    int set_far = ec == HV_EC_PC_ALIGN || ec == HV_EC_IABT_LOWER || ec == HV_EC_IABT_SAME ||
                  ec == HV_EC_DABT_LOWER || ec == HV_EC_DABT_SAME;
    hv_take_exception(cpu, HV_EXC_SYNC, ec << 26 | 1u << 25 | (iss & 0x1ffffff), far, set_far, pc);
}

void hv_raise_undef(HvCpu *cpu, uint64_t pc) {
    static int reported;
    if (cpu->el == 0 && ((pc >> 55) & 1) && reported++ < 3) hv_debug_el0_in_kernel(cpu, "undef");
    if (cpu->m->log) {
        uint32_t insn = 0;
        uint64_t pa;
        if (!hv_mmu_access(cpu, pc, HV_ACC_EXEC, cpu->el, &pa)) hv_phys_read(cpu->m, pa, &insn, 4);
        fprintf(stderr, "undefined: pc %llx insn %08x el%u\n", (unsigned long long)pc, insn, cpu->el);
        if (cpu->el == 1) { /* a kernel assert: its backtrace, from the frame pointers (lr first) */
            fprintf(stderr, "  backtrace: lr %llx", (unsigned long long)(cpu->x[30] | 0xffffff8000000000ull));
            uint64_t fp = cpu->x[29];
            for (int i = 0; i < 24 && fp; i++) {
                uint64_t fr[2], fpa;
                if (hv_mmu_access(cpu, fp, HV_ACC_READ, 1, &fpa) || hv_phys_read(cpu->m, fpa, fr, 16)) break;
                uint64_t ret = fr[1] | 0xffffff8000000000ull; /* strip the PAC bits */
                fprintf(stderr, " %llx", (unsigned long long)ret);
                if (fr[0] <= fp) break;
                fp = fr[0];
            }
            fprintf(stderr, "\n  vbar %llx\n", (unsigned long long)cpu->vbar_el1);
        }
    }
    hv_raise_sync(cpu, HV_EC_UNKNOWN, 0, 0, pc);
}

/* The page tables behind an address that just faulted, printed level by level,
   read as physical memory. A guest that faults on a mapping it believes in is
   either looking at a table we are not (wrong TTBR, wrong base) or one where
   that entry was never written; which level comes up empty says which. */
/* What is actually filled in, in the table that came up short: the runs of
   populated entries, read as physical memory. "one page" (a run right next to
   the miss), "a range starting one page late", and "nothing at all in a table
   that exists" are three different bugs, and they read differently here. */
static void table_runs(HvCpu *cpu, uint64_t tbl, int lvl, uint64_t miss) {
    int total = 0, runs = 0;
    for (int i = 0; i < 512; i++) {
        uint64_t d = 0;
        if (hv_phys_read(cpu->m, tbl + (uint64_t)i * 8, &d, 8) == 0 && d) total++;
    }
    fprintf(stderr, "    L%d table @%llx: %d/512 entries, miss [%u]%s", lvl, (unsigned long long)tbl,
            total, (unsigned)miss, "  runs:");
    int i = 0;
    while (i < 512 && runs < 8) {
        uint64_t d = 0;
        while (i < 512 && (hv_phys_read(cpu->m, tbl + (uint64_t)i * 8, &d, 8) != 0 || !d)) i++;
        if (i >= 512) break;
        int s = i;
        while (i < 512 && hv_phys_read(cpu->m, tbl + (uint64_t)i * 8, &d, 8) == 0 && d) i++;
        fprintf(stderr, " [%d..%d]", s, i - 1);
        runs++;
    }
    if (runs == 8) fprintf(stderr, " ...");
    fprintf(stderr, "\n");
}

static void walk_note_ttbr(HvCpu *cpu, uint64_t va, int listing, uint64_t root0) {
    uint64_t tcr = cpu->tcr_el1;
    int hi = (va >> 55) & 1;
    if (hi && root0) {  /* the caller asked about the low half only */
        fprintf(stderr, "  walk %llx: ttbr1 address, skipped\n", (unsigned long long)va);
        return;
    }
    uint32_t tg = hi ? (tcr >> 30) & 3 : (tcr >> 14) & 3;
    int g = hi ? (tg == 1 ? 14 : tg == 3 ? 16 : 12) : (tg == 2 ? 14 : tg == 1 ? 16 : 12);
    uint32_t tsz = hi ? (tcr >> 16) & 63 : tcr & 63;
    if (tsz < 16) tsz = 16;
    if (tsz > 39) tsz = 39;
    int inputsize = 64 - (int)tsz, stride = g - 3;
    int levels = (inputsize - g + stride - 1) / stride, level = 4 - levels;
    uint64_t base = (hi ? cpu->ttbr1_el1 : (root0 ? root0 : cpu->ttbr0_el1)) & 0x0000ffffffffffffull & ~1ull;
    fprintf(stderr, "  walk %llx: ttbr%d %llx tsz %u gran %d levels %d%s\n", (unsigned long long)va, hi,
            (unsigned long long)base, (unsigned)tsz, g, levels, root0 && !hi ? "  <- previous root" : "");
    uint64_t parent = 0, parent_idx = 0;
    int have_parent = 0;
    for (; level <= 3; level++) {
        int shift = g + stride * (3 - level);
        int idx_bits = level == 4 - levels ? inputsize - shift : stride;
        uint64_t idx = (va >> shift) & ((1ull << idx_bits) - 1), desc = 0;
        if (hv_phys_read(cpu->m, base + idx * 8, &desc, 8) != 0) {
            fprintf(stderr, "    L%d[%u] @%llx = no ram there\n", level, (unsigned)idx,
                    (unsigned long long)(base + idx * 8));
            if (listing) table_runs(cpu, base, level, idx);
            return;
        }
        fprintf(stderr, "    L%d[%u] @%llx = %016llx%s%s\n", level, (unsigned)idx,
                (unsigned long long)(base + idx * 8), (unsigned long long)desc,
                (desc & 1) ? "" : "  <- invalid", (desc & 2) ? " table" : "");
        if (!(desc & 1) || !(desc & 2)) {
            if (listing) {
                table_runs(cpu, base, level, (int)idx);
                hv_pte_report(cpu->m, base, idx);
                /* When did this level's own slot get pointed at that table?
                   A link written a moment ago means the table was born empty
                   and nobody filled it afterwards. */
                if (have_parent) {
                    fprintf(stderr, "    parent L%d[%u] @%llx, its writes:\n", level - 1, (unsigned)parent_idx,
                            (unsigned long long)(parent + parent_idx * 8));
                    hv_pte_report(cpu->m, parent, parent_idx);
                }
            }
            return;
        }
        parent = base, parent_idx = idx, have_parent = 1;
        base = desc & 0x0000fffffffff000ull;
    }
}

static void walk_note(HvCpu *cpu, uint64_t va, int listing) { walk_note_ttbr(cpu, va, listing, 0); }

void hv_raise_abort(HvCpu *cpu, int insn, uint64_t va, uint32_t fsc, int wnr, int s1ptw, uint64_t pc) {
    static int walk_reported, aborts_seen;
    /* Every abort gets one line (the first 200): a fault the guest survived is
       still part of the story of the one it did not. The first four, and only
       translation/permission faults, also get the tables and the PTE history:
       not the ones that fault on a table walk itself (s1ptw), which say nothing
       about the mapping that is missing. */
    if (cpu->m->log && aborts_seen < 200) {
        aborts_seen++;
        fprintf(stderr, "abort#%d %s va %llx fsc %#x pc %llx el%u%s\n", aborts_seen,
                insn ? "fetch" : (wnr ? "write" : "read"), (unsigned long long)va, (unsigned)fsc,
                (unsigned long long)pc, cpu->el, s1ptw ? " (table walk)" : "");
    }
    if (!s1ptw && cpu->m->log && walk_reported < 4 &&
        ((fsc & 0x3c) == 0x04 || (fsc & 0x3c) == 0x0c)) {
        walk_reported++;
        fprintf(stderr, "abort detail %s va %llx fsc %#x pc %llx el%u\n", insn ? "fetch" : (wnr ? "write" : "read"),
                (unsigned long long)va, (unsigned)fsc, (unsigned long long)pc, cpu->el);
        walk_note(cpu, va, 1);
        if (pc != va) walk_note(cpu, pc, 1);
        if (cpu->x[1] && cpu->x[1] != va) walk_note(cpu, cpu->x[1], 0);
        if (cpu->x[0] && cpu->x[0] != va && cpu->x[0] != cpu->x[1]) walk_note(cpu, cpu->x[0], 0);
        /* The same address in the address space we were in before the last
           switch: if the mapping is there and not here, the mapping is not
           missing at all -- it belongs to a root this code is no longer running
           under. */
        if (cpu->ttbr0_prev && cpu->ttbr0_prev != cpu->ttbr0_el1 && !((va >> 55) & 1)) {
            fprintf(stderr, "  same va, previous root %llx:\n", (unsigned long long)cpu->ttbr0_prev);
            walk_note_ttbr(cpu, va, 1, cpu->ttbr0_prev);
        }
        fprintf(stderr, "  state: elr %llx spsr %llx sp %llx x0 %llx x1 %llx x2 %llx"
                        " x19 %llx x20 %llx x21 %llx x29 %llx x30 %llx\n",
                (unsigned long long)cpu->elr_el1, (unsigned long long)cpu->spsr_el1, (unsigned long long)cpu->sp,
                (unsigned long long)cpu->x[0], (unsigned long long)cpu->x[1], (unsigned long long)cpu->x[2],
                (unsigned long long)cpu->x[19], (unsigned long long)cpu->x[20], (unsigned long long)cpu->x[21],
                (unsigned long long)cpu->x[29], (unsigned long long)cpu->x[30]);
    }
    uint32_t ec = insn ? (cpu->el ? HV_EC_IABT_SAME : HV_EC_IABT_LOWER)
                       : (cpu->el ? HV_EC_DABT_SAME : HV_EC_DABT_LOWER);
    uint32_t iss = fsc | (uint32_t)(wnr && !insn) << 6 | (uint32_t)s1ptw << 7;
    hv_raise_sync(cpu, ec, iss, va, pc);
}

void hv_eret(HvCpu *cpu) {
    if (cpu->m) {
        HvStats *st = &cpu->m->stats;
        uint32_t k = st->nelchg++ & 15;
        st->elchg[k].from = cpu->pc, st->elchg[k].to = cpu->elr_el1, st->elchg[k].spsr = cpu->spsr_el1;
        st->elchg[k].kind = 9, st->elchg[k].el = cpu->el;
    }
    cpu->pc = cpu->elr_el1;
    hv_pstate_set(cpu, cpu->spsr_el1);
    cpu->excl_addr = ~0ull;
}

static HvCpu *cpu_by_mpidr(HvMachine *m, uint64_t mpidr) {
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i] && (m->cpus[i]->mpidr & 0xff00ffffffull) == (mpidr & 0xff00ffffffull)) return m->cpus[i];
    return NULL;
}

/* PSCI 1.0. */
static uint64_t psci(HvCpu *cpu, uint64_t fn) {
    static int log = -1;
    if (log < 0) log = getenv("HV_PSCI_LOG") != NULL;
    if (log && ((uint32_t)fn >> 24 == 0x84 || (uint32_t)fn >> 24 == 0xc4))
        fprintf(stderr, "psci %#llx x1 %#llx x2 %#llx x3 %#llx pc %llx\n", (unsigned long long)fn, (unsigned long long)cpu->x[1],
                (unsigned long long)cpu->x[2], (unsigned long long)cpu->x[3], (unsigned long long)cpu->pc);
    switch ((uint32_t)fn) {
    case 0x84000000: return 0x10000;                         /* PSCI_VERSION: 1.0 */
    case 0x84000006: return 2;                               /* MIGRATE_INFO_TYPE: no trusted OS */
    case 0x84000008:                                         /* SYSTEM_OFF */
    case 0x84000009:                                         /* SYSTEM_RESET */
        if (cpu->m->log)
            fprintf(stderr, "hengevm: guest requested %s on cpu %u\n",
                    (uint32_t)fn == 0x84000008 ? "shutdown" : "restart", cpu->index);
        hv_machine_halt(cpu->m, (uint32_t)fn == 0x84000008 ? HV_HALT_SHUTDOWN : HV_HALT_RESET);
        return 0;
    case 0x8400000a: {                                       /* PSCI_FEATURES */
        uint32_t q = (uint32_t)cpu->x[1];
        if (q == 0x84000000 || q == 0x84000002 || q == 0x84000006 || q == 0x84000008 || q == 0x84000009 ||
            q == 0x8400000a || q == 0xc4000003 || q == 0x84000003 || q == 0xc4000004 || q == 0x84000004)
            return 0;
        return (uint64_t)-1;
    }
    case 0x84000002:                                         /* CPU_OFF: until another CPU_ON */
        cpu->off = 1;
        return 0;
    case 0x84000003: case 0xc4000003: {                      /* CPU_ON: target, entry point, context id */
        HvCpu *t = cpu_by_mpidr(cpu->m, cpu->x[1]);
        if (!t) return (uint64_t)-2;                         /* INVALID_PARAMETERS */
        if (!t->off) return (uint64_t)-4;                    /* ALREADY_ON */
        while (!__atomic_load_n(&t->parked_off, __ATOMIC_SEQ_CST)) {
            if (cpu->halted || t->halted) return (uint64_t)-3; /* DENIED: machine is stopping */
            sched_yield(); /* still on its way out of CPU_OFF */
        }
        uint64_t mpidr = t->mpidr, entry = cpu->x[2], context = cpu->x[3];
        if ((uint32_t)fn == 0x84000003) entry = (uint32_t)entry, context = (uint32_t)context;
        /* As at reset: EL1h, MMU and caches off, everything masked. Its thread
           waits on wake_mu, so it cannot see the CPU half reset. */
        pthread_mutex_lock(&t->pc_state->wake_mu);
        HvPerCpu *pcs = t->pc_state;
        if (cpu->halted || t->halted) {
            pthread_mutex_unlock(&pcs->wake_mu);
            return (uint64_t)-3;
        }
        hv_cpu_reset_index(t, cpu->m, t->index);
        t->mpidr = mpidr;
        t->pc = entry;
        t->x[0] = context;
        t->parked_off = 1;
        __atomic_store_n(&t->off, 0, __ATOMIC_RELEASE);
        pcs->wake_pending = 1;
        pthread_cond_signal(&pcs->wake_cv);
        pthread_mutex_unlock(&pcs->wake_mu);
        return 0;
    }
    case 0x84000004: case 0xc4000004: {                      /* AFFINITY_INFO: 0 on, 1 off */
        HvCpu *t = cpu_by_mpidr(cpu->m, cpu->x[1]);
        if (!t) return (uint64_t)-2;
        return t->off ? 1 : 0;
    }
    /* vmapple's paravirtualized PAC (XNU osfmk/arm64/hv_hvc.h): the guest
       cannot program PAC keys itself, so the hypervisor does. HengeVM never
       signs pointers, so there is nothing to program; each call only has to
       succeed (XNU spins on a nonzero x0). */
    case 0xc1000000: /* VMAPPLE_PAC_SET_INITIAL_STATE */
    case 0xc1000002: /* VMAPPLE_PAC_SET_A_KEYS */
    case 0xc1000003: /* VMAPPLE_PAC_SET_B_KEYS */
    case 0xc1000004: /* VMAPPLE_PAC_SET_EL0_DIVERSIFIER */
    case 0xc1000005: /* VMAPPLE_PAC_SET_EL0_DIVERSIFIER_AT_EL1 */
    case 0xc1000006: /* VMAPPLE_PAC_SET_G_KEY */
    case 0xc10000f0: /* VMAPPLE_PAC_NOP */
        return 0;
    case 0xc1000001: /* VMAPPLE_PAC_GET_DEFAULT_KEYS: x2 = default ROP id, x3 = default JOP id */
        cpu->x[2] = 0x524f50; /* the values only name keys, and no key is ever used */
        cpu->x[3] = 0x4a4f50;
        return 0;
    default:
        if (cpu->m->log) fprintf(stderr, "hengevm: unknown hypervisor call %#llx (x1 %#llx) at el%u\n",
                                 (unsigned long long)fn, (unsigned long long)cpu->x[1], cpu->el);
        return (uint64_t)-1;                                 /* NOT_SUPPORTED */
    }
}

/* Bring-up aid: x0-x3, and any string they point to (read with the current EL's mapping). */
void hv_debug_args(HvCpu *cpu) {
    for (int i = 0; i < 4; i++) fprintf(stderr, " x%d=%llx", i, (unsigned long long)cpu->x[i]);
    fprintf(stderr, "\n");
    for (int i = 0; i < 4; i++) {
        char str[161];
        int n = 0;
        for (; n < 160; n++) {
            uint64_t pa;
            if (hv_mmu_access(cpu, cpu->x[i] + n, HV_ACC_READ, cpu->el, &pa) || hv_phys_read(cpu->m, pa, &str[n], 1) ||
                str[n] < 9 || (unsigned char)str[n] > 126)
                break;
        }
        str[n] = 0;
        if (n >= 4) fprintf(stderr, "    x%d -> \"%s\"\n", i, str);
    }
}

int hv_exception_insn(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    uint32_t imm16 = (insn >> 5) & 0xffff;
    uint32_t opc = (insn >> 21) & 7, ll = insn & 3;
    if (opc == 0 && ll == 1 && cpu->m->log >= 3) {
        fprintf(stderr, "svc #%x at %llx el%u:", imm16, (unsigned long long)pc, cpu->el);
        hv_debug_args(cpu);
    }
    if (opc == 0 && ll == 1) { /* SVC: ELR is the next instruction */
        hv_take_exception(cpu, HV_EXC_SYNC, HV_EC_SVC64 << 26 | 1u << 25 | imm16, 0, 0, pc + 4);
        return 1;
    }
    if (opc == 0 && ll == 2 && cpu->el == 1) { /* HVC: the hypervisor's PSCI and vmapple's PAC calls */
        cpu->x[0] = psci(cpu, cpu->x[0]);
        if (!cpu->halted && !cpu->off) return 0; /* like a call: the block carries on (xnu makes one per context switch) */
        cpu->pc = pc + 4;
        cpu->exit_reason = cpu->halted ? HV_EXIT_HALT : HV_EXIT_BRANCH; /* CPU_OFF: the dispatcher parks it */
        return 1;
    }
    if (opc == 1 && ll == 0) { /* BRK */
        hv_take_exception(cpu, HV_EXC_SYNC, HV_EC_BRK << 26 | 1u << 25 | imm16, 0, 0, pc);
        return 1;
    }
    /* SMC with no EL3, HVC from EL0, HLT without halting debug, DCPS: undefined. */
    hv_raise_undef(cpu, pc);
    return 1;
}

int hv_check_interrupts(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    (void)m;
    if (p->fiq_line && !(cpu->daif & HV_PSTATE_F)) {
        hv_take_exception(cpu, HV_EXC_FIQ, 0, 0, 0, cpu->pc);
        return 1;
    }
    if (p->irq_line && !(cpu->daif & HV_PSTATE_I)) {
        hv_take_exception(cpu, HV_EXC_IRQ, 0, 0, 0, cpu->pc);
        return 1;
    }
    return 0;
}
