#ifndef MTRR_CORE_H
#define MTRR_CORE_H

typedef unsigned int MTRRU32;
typedef unsigned long long MTRRU64;

#define MTRR_TYPE_UC 0U
#define MTRR_TYPE_WC 1U

#define MTRR_CAP_MSR 0x000000feU
#define MTRR_PHYSBASE0_MSR 0x00000200U
#define MTRR_PHYSMASK0_MSR 0x00000201U
#define MTRR_DEF_TYPE_MSR 0x000002ffU

#define MTRR_CAP_VARIABLE_COUNT_MASK 0xffULL
#define MTRR_CAP_WRITE_COMBINING (1ULL << 10)
#define MTRR_DEF_TYPE_ENABLE (1ULL << 11)
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
    MTRR_REPACK_VALID = 0,
    MTRR_REPACK_SOURCE_INVALID = 1,
    MTRR_REPACK_SOURCE_NOT_UC = 2,
    MTRR_REPACK_REQUEST_INVALID = 3,
    MTRR_REPACK_REQUEST_NOT_CONTAINED = 4,
    MTRR_REPACK_TOO_MANY_RANGES = 5,
    MTRR_REPACK_BUILD_FAILED = 6,
    MTRR_REPACK_VERIFICATION_FAILED = 7
} MTRRRepackStatus;

typedef struct {
    MTRRU64 base;
    MTRRU64 size;
    MTRRU64 end;
    MTRRU64 rawBase;
    MTRRU64 rawMask;
    unsigned type;
} MTRRPlannedRange;

typedef struct {
    MTRRPlannedRange ranges[MTRR_REPACK_MAX_RANGES];
    unsigned rangeCount;
} MTRRRepackPlan;

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
MTRRRepackStatus MTRRCorePlanUCRepack(const MTRRRange *source,
                                      MTRRU64 requestBase,
                                      MTRRU64 requestSize,
                                      unsigned physicalAddressBits,
                                      MTRRRepackPlan *plan);
const char *MTRRCoreRepackStatusName(MTRRRepackStatus status);

#endif
