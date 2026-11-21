#include "sysreg.h"

#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>

#include "exception.h"
#include "machine.h"
#include "memop.h"
#include "mmu.h"
#include <stdlib.h>

/* System registers are named by op0:op1:CRn:CRm:op2, which is bits 20..5 of MRS/MSR. */
#define R(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))

/* ID registers: only features the translator handles. Host features are a
   superset (Apple M1 and later), so copied instructions always exist. */
/* HV_PFR0_EL3=1 (experiment): report EL3 present in the EL3 field (bits 15:12).
   ArmVirtQemu's earliest stage reads this to decide whether PSCI goes over SMC
   or HVC, before it has parsed the device tree; with no EL3 it takes a path
   that waits for an interrupt nothing sends. */
#define ID_AA64PFR0  0x1100000001110011ull /* EL0/EL1 AArch64, FP+AdvSIMD with FP16, GICv3 sysregs, CSV2, CSV3 */
#define ID_AA64ISAR0 0x0011100110212120ull /* AES+PMULL, SHA1, SHA2/512, CRC32, LSE, RDM, SHA3, DotProd, FHM, FlagM */
/* PAuth is not reported: Windows 11 ARM64's ntoskrnl takes a path for ARMv8.3
   pointer authentication that this translator does not implement, and with the
   field clear it uses plain pointers instead. Everything else is what the
   instructions here actually support (LRCPC2 matters: Prism's xtajit64 issues
   LDAPUR/STLUR and checks it). */
#define ID_AA64ISAR1 0x0000001100211001ull /* DPB, JSCVT, FCMA, LRCPC2, FRINTTS, SB (no PAuth) */
#define ID_AA64MMFR0 0x0000000000100025ull /* 48-bit PA, 16-bit ASID, 4K/16K/64K granules */
#define ID_AA64MMFR1 0x0000000000111000ull /* HPDS, LOR, PAN */
#define ID_AA64DFR0  0x0000000000000006ull /* Armv8 debug, no PMU */
/* Cortex-A76 r4p1. Windows 11 ARM64's ntoskrnl reads MIDR to pick its CPU
   workarounds and errata handling, and an implementer code it does not know
   (Apple's 0x61) is treated as an unsupported CPU. This is what QEMU's virt
   reports to a Windows guest, and what the instructions translated here
   support exactly. */
#define MIDR         0x414fd0b1ull
#define CTR_EL0      0x9444c004ull         /* 64-byte lines, PIPT I-cache, IDC */
#define CLIDR        0x0a200023ull         /* L1 split, L2 unified */
#define CNTFRQ       24000000ull

static uint64_t counter(void) {
    return mach_absolute_time(); /* 24 MHz on Apple silicon */
}

uint64_t hv_counter(void) {
    return counter();
}

static int undefined(HvCpu *cpu, uint64_t pc) {
    hv_raise_undef(cpu, pc);
    return 1;
}

/* Timer control: ISTATUS (bit 2) reflects the compare right now. */
static uint64_t timer_ctl(uint64_t ctl, uint64_t cval, uint64_t now) {
    ctl &= 3;
    if ((ctl & 1) && now >= cval) ctl |= 4;
    return ctl;
}

/* EL0 is allowed only a few registers, some gated by SCTLR_EL1 / CNTKCTL_EL1. */
static int el0_allowed(HvCpu *cpu, uint32_t reg, int write) {
    uint64_t k = cpu->cntkctl_el1;
    switch (reg) {
    case R(3, 3, 4, 2, 0): case R(3, 3, 4, 4, 0): case R(3, 3, 4, 4, 1): /* NZCV, FPCR, FPSR */
    case R(3, 3, 13, 0, 2):                                               /* TPIDR_EL0 */
    case R(3, 3, 4, 2, 5): case R(3, 3, 4, 2, 6):                         /* DIT, SSBS */
        return 1;
    case R(3, 3, 4, 2, 1): return (cpu->sctlr_el1 >> 9) & 1;            /* DAIF: UMA */
    case R(3, 3, 13, 0, 3): return !write;                               /* TPIDRRO_EL0 */
    case R(3, 3, 0, 0, 1): return !write && ((cpu->sctlr_el1 >> 15) & 1); /* CTR_EL0: UCT */
    case R(3, 3, 0, 0, 7): return !write;                                /* DCZID_EL0 */
    case R(3, 3, 14, 0, 0): return !write && (k & 3);                    /* CNTFRQ */
    case R(3, 3, 14, 0, 1): case R(3, 3, 14, 0, 5): return !write && (k & 1);       /* CNTPCT */
    case R(3, 3, 14, 0, 2): case R(3, 3, 14, 0, 6): return !write && ((k >> 1) & 1); /* CNTVCT */
    case R(3, 3, 14, 3, 0): case R(3, 3, 14, 3, 1): case R(3, 3, 14, 3, 2): return (k >> 8) & 1; /* CNTV_* */
    case R(3, 3, 14, 2, 0): case R(3, 3, 14, 2, 1): case R(3, 3, 14, 2, 2): return (k >> 9) & 1; /* CNTP_* */
    default: return 0;
    }
}

/* Returns 0 and sets *v, or -1 if the register does not exist. */
static int read_reg(HvCpu *cpu, uint32_t reg, uint64_t *v) {
    uint64_t now = counter();
    HvMachine *m = cpu->m;
    switch (reg) {
    case R(3, 3, 4, 2, 0): *v = cpu->nzcv & 0xf0000000; return 0;
    case R(3, 3, 4, 2, 1): *v = cpu->daif; return 0;
    case R(3, 0, 4, 2, 2): *v = (uint64_t)cpu->el << 2; return 0;      /* CurrentEL */
    case R(3, 0, 4, 2, 0): *v = cpu->spsel; return 0;
    case R(3, 0, 4, 2, 3): *v = cpu->pstate_extra & (1u << 22); return 0; /* PAN */
    case R(3, 0, 4, 2, 4): *v = cpu->pstate_extra & (1u << 23); return 0; /* UAO */
    case R(3, 3, 4, 2, 5): *v = cpu->pstate_extra & (1u << 24); return 0; /* DIT */
    case R(3, 3, 4, 2, 6): *v = cpu->pstate_extra & (1u << 12); return 0; /* SSBS */
    /* FPCR/FPSR live in the host registers while the guest runs (hv_fp_enter). */
    case R(3, 3, 4, 4, 0): __asm__ volatile("mrs %0, fpcr" : "=r"(*v)); return 0;
    case R(3, 3, 4, 4, 1): __asm__ volatile("mrs %0, fpsr" : "=r"(*v)); return 0;
    case R(3, 0, 4, 1, 0): *v = cpu->spsel ? cpu->sp_el0 : cpu->sp; return 0; /* SP_EL0 */
    case R(3, 0, 4, 0, 0): *v = cpu->spsr_el1; return 0;
    case R(3, 0, 4, 0, 1): *v = cpu->elr_el1; return 0;
    case R(3, 0, 1, 0, 0): *v = cpu->sctlr_el1; return 0;
    case R(3, 0, 1, 0, 1): *v = cpu->actlr_el1; return 0;
    case R(3, 0, 1, 0, 2): *v = cpu->cpacr_el1; return 0;
    case R(3, 0, 2, 0, 0): *v = cpu->ttbr0_el1; return 0;
    case R(3, 0, 2, 0, 1): *v = cpu->ttbr1_el1; return 0;
    case R(3, 0, 2, 0, 2): *v = cpu->tcr_el1; return 0;
    case R(3, 0, 5, 1, 0): *v = cpu->afsr0_el1; return 0;
    case R(3, 0, 5, 1, 1): *v = cpu->afsr1_el1; return 0;
    case R(3, 0, 5, 2, 0): *v = cpu->esr_el1; return 0;
    case R(3, 0, 6, 0, 0): *v = cpu->far_el1; return 0;
    case R(3, 0, 7, 4, 0): *v = cpu->par_el1; return 0;
    case R(3, 0, 10, 2, 0): *v = cpu->mair_el1; return 0;
    case R(3, 0, 10, 3, 0): *v = cpu->amair_el1; return 0;
    case R(3, 0, 12, 0, 0): *v = cpu->vbar_el1; return 0;
    case R(3, 0, 12, 1, 0): /* ISR_EL1: interrupts pending at the CPU, masked or not */
        *v = (cpu->pc_state && cpu->pc_state->irq_line ? 1u << 7 : 0) | (cpu->pc_state && cpu->pc_state->fiq_line ? 1u << 6 : 0);
        return 0;
    case R(3, 0, 13, 0, 1): *v = cpu->contextidr_el1; return 0;
    case R(3, 0, 13, 0, 4): *v = cpu->tpidr_el1; return 0;
    case R(3, 3, 13, 0, 2): *v = cpu->tpidr_el0; return 0;
    case R(3, 3, 13, 0, 3): *v = cpu->tpidrro_el0; return 0;
    case R(3, 0, 14, 1, 0): *v = cpu->cntkctl_el1; return 0;
    case R(2, 0, 0, 2, 2): *v = cpu->mdscr_el1; return 0;
    case R(2, 0, 1, 1, 4): *v = 0x8; return 0;                           /* OSLSR_EL1: OS lock implemented, unlocked */
    case R(3, 2, 0, 0, 0): *v = cpu->csselr_el1; return 0;

    /* Identification */
    case R(3, 0, 0, 0, 0): *v = MIDR; return 0;
    case R(3, 0, 0, 0, 5): *v = cpu->mpidr; return 0;
    case R(3, 0, 0, 0, 6): *v = 0; return 0;                             /* REVIDR */
    case R(3, 0, 0, 4, 0):
        *v = ID_AA64PFR0;
        {
            static int el3 = -1;
            if (el3 < 0) el3 = getenv("HV_PFR0_EL3") != NULL;
            if (el3) *v |= (uint64_t)2 << 12; /* EL3: AArch64 */
        }
        return 0;
    case R(3, 0, 0, 5, 0): *v = ID_AA64DFR0; return 0;
    case R(3, 0, 0, 6, 0): *v = ID_AA64ISAR0; return 0;
    case R(3, 0, 0, 6, 1): *v = ID_AA64ISAR1; return 0;
    case R(3, 0, 0, 7, 0): *v = ID_AA64MMFR0; return 0;
    case R(3, 0, 0, 7, 1): *v = ID_AA64MMFR1; return 0;
    case R(3, 3, 0, 0, 1): *v = CTR_EL0; return 0;
    case R(3, 3, 0, 0, 7): *v = 4; return 0;                             /* DCZID: 64-byte DC ZVA */
    case R(3, 1, 0, 0, 1): *v = CLIDR; return 0;
    case R(3, 1, 0, 0, 7): *v = 0; return 0;                             /* AIDR */
    case R(3, 1, 0, 0, 0):                                               /* CCSIDR for CSSELR's level */
        *v = (cpu->csselr_el1 >> 1) == 0 ? (127u << 13 | 7u << 3 | 2) : (2047u << 13 | 15u << 3 | 2);
        return 0;

    /* Generic timer */
    case R(3, 3, 14, 0, 0): *v = CNTFRQ; return 0;
    case R(3, 3, 14, 0, 1): case R(3, 3, 14, 0, 5): *v = now; return 0;
    case R(3, 3, 14, 0, 2): case R(3, 3, 14, 0, 6): *v = now - cpu->cntvoff; return 0;
    case R(3, 3, 14, 2, 0): *v = (uint32_t)(cpu->cntp_cval - now); return 0;
    case R(3, 3, 14, 2, 1): *v = timer_ctl(cpu->cntp_ctl, cpu->cntp_cval, now); return 0;
    case R(3, 3, 14, 2, 2): *v = cpu->cntp_cval; return 0;
    case R(3, 3, 14, 3, 0): *v = (uint32_t)(cpu->cntv_cval - (now - cpu->cntvoff)); return 0;
    case R(3, 3, 14, 3, 1): *v = timer_ctl(cpu->cntv_ctl, cpu->cntv_cval, now - cpu->cntvoff); return 0;
    case R(3, 3, 14, 3, 2): *v = cpu->cntv_cval; return 0;
    /* PMCCNTR_EL0: bootmgfw reads it 400-odd times to time its own boot, so it
       must advance. The virtual counter is the same monotonic 24 MHz the guest
       timers already run on; nothing here implements the cycle counter's
       enable bits, so it counts from reset. */
    case R(3, 3, 9, 13, 0): *v = counter(); return 0;
    }
    /* GIC CPU interface (ICC_*_EL1): CRn 12, CRm 8..12, and ICC_PMR_EL1. */
    if (reg == R(3, 0, 4, 6, 0) || ((reg >> 14) == 3 && ((reg >> 11) & 7) == 0 && ((reg >> 7) & 15) == 12 && ((reg >> 3) & 15) >= 8)) {
        *v = m->icc_read ? m->icc_read(m, cpu, reg) : 0; /* the GIC locks itself */
        return 0;
    }
    /* The rest of the ID space reads as zero; so do debug and PMU registers. */
    if ((reg >> 14) == 3 && ((reg >> 11) & 7) == 0 && ((reg >> 7) & 15) == 0 && ((reg >> 3) & 15) >= 1) {
        *v = 0;
        return 0;
    }
    /* Debug and PMU registers (op0 3, CRn 9: PMCR, PMCNTEN*, PMOVS*, PMUSERENR;
       PMCCNTR is above) read as zero, and so does the whole PMU event space at
       CRn 14, CRm 8..15 (PMEVCNTR<n>, PMEVTYPER<n>, and PMCCFILTR_EL0, which is
       S3_3_C14_C15_7). bootmgfw writes PMCCFILTR while it sets the counters up,
       and a fault on that instruction is the whole of this boot path: it takes
       the exception at VBAR_EL1+0x200, where its stub vector is a `b .`. */
    if ((reg >> 14) == 2 || (reg >> 14) == 3 && (((reg >> 7) & 15) == 9 ||
                                                (((reg >> 7) & 15) == 14 && ((reg >> 3) & 15) >= 8))) {
        *v = 0;
        return 0;
    }
    /* Apple implementation-defined registers (op0 3, CRn 15). The vmapple
       kernel reads error-status registers there (S3_6_C15_C0_0, S3_3_C15_C8_0..)
       when it reports a machine error; zero means no error recorded. */
    if ((reg >> 14) == 3 && ((reg >> 7) & 15) == 15) {
        if (m->log) fprintf(stderr, "hengevm: read of implementation-defined S3_%u_C15_C%u_%u = 0\n", (reg >> 11) & 7,
                            (reg >> 3) & 15, reg & 7);
        *v = 0;
        return 0;
    }
    return -1;
}

static int write_reg(HvCpu *cpu, uint32_t reg, uint64_t v) {
    HvMachine *m = cpu->m;
