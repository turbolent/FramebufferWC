#ifndef MTRR_X86_H
#define MTRR_X86_H

#include "MTRRCore.h"

typedef struct {
    MTRRU32 eax;
    MTRRU32 ebx;
    MTRRU32 ecx;
    MTRRU32 edx;
} MTRRCPUIDResult;

typedef struct {
    unsigned index;
    MTRRU64 rawBase;
    MTRRU64 rawMask;
} MTRRX86VariableRangeWrite;

int MTRRX86CPUIDAvailable(void);
void MTRRX86CPUID(MTRRU32 leaf, MTRRU32 subleaf,
                  MTRRCPUIDResult *result);
MTRRU64 MTRRX86ReadMSR(MTRRU32 msr);
void MTRRX86ProgramVariableRanges(
    const MTRRX86VariableRangeWrite *writes,
    unsigned writeCount,
    MTRRU64 rawDefaultType);

#endif
