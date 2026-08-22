#include "MTRRX86.h"

int
MTRRX86CPUIDAvailable(void)
{
    MTRRU32 before;
    MTRRU32 after;

    __asm__ volatile (
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0,%1\n\t"
        "xorl $0x00200000,%1\n\t"
        "pushl %1\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %1\n\t"
        "pushl %0\n\t"
        "popfl"
        : "=&r" (before), "=&r" (after)
        :
        : "cc", "memory");
    return ((before ^ after) & 0x00200000U) != 0;
}

void
MTRRX86CPUID(MTRRU32 leaf, MTRRU32 subleaf, MTRRCPUIDResult *result)
{
    __asm__ volatile (
        "cpuid"
        : "=a" (result->eax), "=b" (result->ebx),
          "=c" (result->ecx), "=d" (result->edx)
        : "0" (leaf), "2" (subleaf)
        : "memory");
}

MTRRU64
MTRRX86ReadMSR(MTRRU32 msr)
{
    MTRRU32 low;
    MTRRU32 high;

    __asm__ volatile (
        "rdmsr"
        : "=a" (low), "=d" (high)
        : "c" (msr)
        : "memory");
    return (MTRRU64)low | ((MTRRU64)high << 32);
}

#define MTRR_X86_CR0_NW (1U << 29)
#define MTRR_X86_CR0_CD (1U << 30)
#define MTRR_X86_CR4_PGE (1U << 7)

static void
MTRRX86WriteMSR(MTRRU32 msr, MTRRU64 value)
{
    MTRRU32 low;
    MTRRU32 high;

    low = (MTRRU32)value;
    high = (MTRRU32)(value >> 32);
    __asm__ volatile (
        "wrmsr"
        :
        : "c" (msr), "a" (low), "d" (high)
        : "memory");
}

static MTRRU32
MTRRX86ReadCR0(void)
{
    MTRRU32 value;

    __asm__ volatile ("movl %%cr0,%0" : "=r" (value) : : "memory");
    return value;
}

static void
MTRRX86WriteCR0(MTRRU32 value)
{
    __asm__ volatile ("movl %0,%%cr0" : : "r" (value) : "memory");
}

static MTRRU32
MTRRX86ReadCR3(void)
{
    MTRRU32 value;

    __asm__ volatile ("movl %%cr3,%0" : "=r" (value) : : "memory");
    return value;
}

static void
MTRRX86WriteCR3(MTRRU32 value)
{
    __asm__ volatile ("movl %0,%%cr3" : : "r" (value) : "memory");
}

static MTRRU32
MTRRX86ReadCR4(void)
{
    MTRRU32 value;

    __asm__ volatile ("movl %%cr4,%0" : "=r" (value) : : "memory");
    return value;
}

static void
MTRRX86WriteCR4(MTRRU32 value)
{
    __asm__ volatile ("movl %0,%%cr4" : : "r" (value) : "memory");
}

static MTRRU32
MTRRX86DisableInterrupts(void)
{
    MTRRU32 flags;

    __asm__ volatile (
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r" (flags)
        :
        : "cc", "memory");
    return flags;
}

static void
MTRRX86RestoreInterrupts(MTRRU32 flags)
{
    __asm__ volatile (
        "pushl %0\n\t"
        "popfl"
        :
        : "r" (flags)
        : "cc", "memory");
}

static void
MTRRX86WriteBackInvalidate(void)
{
    __asm__ volatile ("wbinvd" : : : "memory");
}

void
MTRRX86ProgramVariableRanges(const MTRRX86VariableRangeWrite *writes,
                             unsigned writeCount,
                             MTRRU64 rawDefaultType)
{
    MTRRU32 savedFlags;
    MTRRU32 savedCR0;
    MTRRU32 savedCR3;
    MTRRU32 savedCR4;
    MTRRU32 noFillCR0;
    unsigned position;

    /*
     * Intel SDM Vol. 3A, MemTypeSet pre_mtrr_change/post_mtrr_change.
     * This routine is deliberately local-CPU only.  Its caller must reject
     * every system with more than one available processor.
     */
    savedFlags = MTRRX86DisableInterrupts();
    savedCR0 = MTRRX86ReadCR0();
    savedCR3 = MTRRX86ReadCR3();
    savedCR4 = MTRRX86ReadCR4();

    noFillCR0 = (savedCR0 | MTRR_X86_CR0_CD) & ~MTRR_X86_CR0_NW;
    MTRRX86WriteCR0(noFillCR0);
    MTRRX86WriteBackInvalidate();

    if (savedCR4 & MTRR_X86_CR4_PGE)
        MTRRX86WriteCR4(savedCR4 & ~MTRR_X86_CR4_PGE);
    MTRRX86WriteCR3(savedCR3);

    MTRRX86WriteMSR(MTRR_DEF_TYPE_MSR,
                    rawDefaultType & ~MTRR_DEF_TYPE_ENABLE);

    for (position = 0; position < writeCount; position++) {
        MTRRU32 maskMSR;

        maskMSR = MTRR_PHYSMASK0_MSR + writes[position].index * 2U;
        MTRRX86WriteMSR(maskMSR,
                        writes[position].rawMask & ~MTRR_PHYSMASK_VALID);
    }
    for (position = 0; position < writeCount; position++) {
        MTRRU32 baseMSR;

        baseMSR = MTRR_PHYSBASE0_MSR + writes[position].index * 2U;
        MTRRX86WriteMSR(baseMSR, writes[position].rawBase);
    }
    for (position = 0; position < writeCount; position++) {
        MTRRU32 maskMSR;

        maskMSR = MTRR_PHYSMASK0_MSR + writes[position].index * 2U;
        MTRRX86WriteMSR(maskMSR, writes[position].rawMask);
    }

    MTRRX86WriteBackInvalidate();
    MTRRX86WriteCR3(savedCR3);
    MTRRX86WriteMSR(MTRR_DEF_TYPE_MSR, rawDefaultType);
    MTRRX86WriteCR0(savedCR0);
    MTRRX86WriteCR4(savedCR4);
    MTRRX86RestoreInterrupts(savedFlags);
}
