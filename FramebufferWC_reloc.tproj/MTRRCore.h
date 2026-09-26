#ifndef MTRR_CORE_H
#define MTRR_CORE_H

typedef unsigned int MTRRU32;
typedef unsigned long long MTRRU64;

#define MTRR_TYPE_UC 0U
#define MTRR_TYPE_WC 1U
#define MTRR_TYPE_WT 4U
#define MTRR_TYPE_WP 5U
#define MTRR_TYPE_WB 6U

#define MTRR_CAP_MSR 0x000000feU
#define MTRR_PHYSBASE0_MSR 0x00000200U
#define MTRR_PHYSMASK0_MSR 0x00000201U
#define MTRR_DEF_TYPE_MSR 0x000002ffU

#define MTRR_CAP_VARIABLE_COUNT_MASK 0xffULL
#define MTRR_CAP_WRITE_COMBINING (1ULL << 10)
#define MTRR_DEF_TYPE_ENABLE (1ULL << 11)
#define MTRR_DEF_TYPE_FIXED_ENABLE (1ULL << 10)
#define MTRR_PHYSMASK_VALID (1ULL << 11)
#define MTRR_REPACK_MAX_RANGES 32U

typedef enum {
    MTRR_RANGE_UNUSED = 0,
    MTRR_RANGE_VALID = 1,
    MTRR_RANGE_MALFORMED = 2,
    MTRR_RANGE_UNDECODABLE = 3
} MTRRRangeStatus;

typedef struct {
    MTRRU64 rawBase;
    MTRRU64 rawMask;
    MTRRU64 base;
    MTRRU64 size;
    MTRRU64 end;
    unsigned type;
    MTRRRangeStatus status;
} MTRRRange;

typedef enum {
    MTRR_REQUEST_VALID = 0,
    MTRR_REQUEST_SIZE_TOO_SMALL = 1,
    MTRR_REQUEST_SIZE_NOT_POWER_OF_TWO = 2,
    MTRR_REQUEST_BASE_MISALIGNED = 3,
    MTRR_REQUEST_OVERFLOW = 4,
    MTRR_REQUEST_OUTSIDE_ADDRESS_WIDTH = 5,
    MTRR_REQUEST_ABOVE_32BIT_OS_LIMIT = 6
} MTRRRequestStatus;

typedef enum {
    MTRR_WC_VALID = 0,
    MTRR_WC_ALREADY_SET,
    MTRR_WC_INVALID_REQUEST,
    MTRR_WC_INVALID_LAYOUT,
    MTRR_WC_LOW_MEMORY,
    MTRR_WC_NO_SPACE,
    MTRR_WC_VERIFICATION_FAILED
} MTRRWCStatus;

/* Complete register table, retaining the indices of unaffected entries. */
typedef struct {
    MTRRRange ranges[MTRR_REPACK_MAX_RANGES];
    unsigned changedCount;
    unsigned requiredCount;
} MTRRWCPlan;

MTRRWCStatus MTRRCorePlanFramebufferWC(const MTRRRange *ranges,
                                       unsigned rangeCount,
                                       MTRRU64 requestBase,
                                       MTRRU64 requestSize,
                                       unsigned physicalAddressBits,
                                       int fixedRangesEnabled,
                                       MTRRWCPlan *plan);
const char *MTRRCoreWCStatusName(MTRRWCStatus status);

MTRRRangeStatus MTRRCoreDecodeRange(MTRRU64 rawBase,
                                    MTRRU64 rawMask,
                                    unsigned physicalAddressBits,
                                    MTRRRange *result);
MTRRRequestStatus MTRRCoreValidateRequest(MTRRU64 base,
                                          MTRRU64 size,
                                          unsigned physicalAddressBits,
                                          MTRRU64 *end);
const char *MTRRCoreRequestStatusName(MTRRRequestStatus status);
int MTRRCoreRangesOverlap(MTRRU64 firstBase, MTRRU64 firstEnd,
                          MTRRU64 secondBase, MTRRU64 secondEnd);
MTRRRequestStatus MTRRCoreBuildRange(MTRRU64 base,
                                     MTRRU64 size,
                                     unsigned type,
                                     unsigned physicalAddressBits,
                                     MTRRU64 *rawBase,
                                     MTRRU64 *rawMask);
#endif
