#include "MTRRCore.h"

static void
clearRange(MTRRRange *range)
{
    range->rawBase = 0;
    range->rawMask = 0;
    range->base = 0;
    range->size = 0;
    range->end = 0;
    range->type = 0;
    range->status = MTRR_RANGE_UNDECODABLE;
}

static int
physicalAddressMask(unsigned physicalAddressBits, MTRRU64 *result)
{
    if (physicalAddressBits < 12U || physicalAddressBits > 52U)
        return 0;
    *result = ((1ULL << physicalAddressBits) - 1ULL) & ~0xfffULL;
    return 1;
}

MTRRRangeStatus
MTRRCoreDecodeRange(MTRRU64 rawBase, MTRRU64 rawMask,
                    unsigned physicalAddressBits, MTRRRange *result)
{
    MTRRU64 addressMask;
    MTRRU64 encodedMask;
    MTRRU64 inverseMask;

    clearRange(result);
    result->rawBase = rawBase;
    result->rawMask = rawMask;
    result->type = (unsigned)(rawBase & 0xffULL);

    if ((rawMask & MTRR_PHYSMASK_VALID) == 0) {
        result->status = MTRR_RANGE_UNUSED;
        return result->status;
    }
    if (!physicalAddressMask(physicalAddressBits, &addressMask)) {
        result->status = MTRR_RANGE_UNDECODABLE;
        return result->status;
    }

    result->base = rawBase & addressMask;
    encodedMask = rawMask & addressMask;
    inverseMask = (~encodedMask) & addressMask;
    result->size = inverseMask + 0x1000ULL;

    if (result->size < 0x1000ULL ||
        (result->size & (result->size - 1ULL)) != 0 ||
        (result->base & (result->size - 1ULL)) != 0 ||
        result->base + result->size - 1ULL < result->base) {
        result->status = MTRR_RANGE_MALFORMED;
        result->end = 0;
        return result->status;
    }

    result->end = result->base + result->size - 1ULL;
    result->status = MTRR_RANGE_VALID;
    return result->status;
}

MTRRRequestStatus
MTRRCoreValidateRequest(MTRRU64 base, MTRRU64 size,
                        unsigned physicalAddressBits, MTRRU64 *end)
{
    MTRRU64 addressMask;
    MTRRU64 physicalLimit;
    MTRRU64 last;

    if (size < 0x1000ULL)
        return MTRR_REQUEST_SIZE_TOO_SMALL;
    if ((size & (size - 1ULL)) != 0)
        return MTRR_REQUEST_SIZE_NOT_POWER_OF_TWO;
    last = base + size - 1ULL;
    if (last < base)
        return MTRR_REQUEST_OVERFLOW;
    if ((base & (size - 1ULL)) != 0)
        return MTRR_REQUEST_BASE_MISALIGNED;
    if (!physicalAddressMask(physicalAddressBits, &addressMask))
        return MTRR_REQUEST_OUTSIDE_ADDRESS_WIDTH;
    physicalLimit = addressMask | 0xfffULL;
    if (base > physicalLimit || last > physicalLimit)
        return MTRR_REQUEST_OUTSIDE_ADDRESS_WIDTH;
    if (last > 0xffffffffULL)
        return MTRR_REQUEST_ABOVE_32BIT_OS_LIMIT;
    if (end != 0)
        *end = last;
    return MTRR_REQUEST_VALID;
}

const char *
MTRRCoreRequestStatusName(MTRRRequestStatus status)
{
    switch (status) {
    case MTRR_REQUEST_VALID:
        return "valid";
    case MTRR_REQUEST_SIZE_TOO_SMALL:
        return "size is below 4096 bytes";
    case MTRR_REQUEST_SIZE_NOT_POWER_OF_TWO:
        return "size is not a power of two";
    case MTRR_REQUEST_BASE_MISALIGNED:
        return "base is not aligned to size";
    case MTRR_REQUEST_OVERFLOW:
        return "range overflows";
    case MTRR_REQUEST_OUTSIDE_ADDRESS_WIDTH:
        return "range exceeds the physical address width";
    case MTRR_REQUEST_ABOVE_32BIT_OS_LIMIT:
        return "range exceeds the 32-bit OS address limit";
    default:
        return "unknown validation error";
    }
}

int
MTRRCoreRangesOverlap(MTRRU64 firstBase, MTRRU64 firstEnd,
                      MTRRU64 secondBase, MTRRU64 secondEnd)
{
    return firstBase <= secondEnd && secondBase <= firstEnd;
}

MTRRRequestStatus
MTRRCoreBuildRange(MTRRU64 base, MTRRU64 size, unsigned type,
                   unsigned physicalAddressBits, MTRRU64 *rawBase,
                   MTRRU64 *rawMask)
{
    MTRRRequestStatus status;
    MTRRU64 addressMask;

    status = MTRRCoreValidateRequest(base, size, physicalAddressBits, 0);
    if (status != MTRR_REQUEST_VALID)
        return status;
    physicalAddressMask(physicalAddressBits, &addressMask);
    if (rawBase != 0)
        *rawBase = (base & addressMask) | (type & 0xffU);
    if (rawMask != 0)
        *rawMask = ((~(size - 1ULL)) & addressMask) |
                   MTRR_PHYSMASK_VALID;
    return MTRR_REQUEST_VALID;
}

static int
appendPlannedRange(MTRRRepackPlan *plan, MTRRU64 base, MTRRU64 size,
                   unsigned type, unsigned physicalAddressBits)
{
    MTRRPlannedRange *range;

    if (plan->rangeCount >= MTRR_REPACK_MAX_RANGES)
        return 0;
    range = &plan->ranges[plan->rangeCount];
    range->base = base;
    range->size = size;
    range->end = base + size - 1ULL;
    range->type = type;
    if (MTRRCoreBuildRange(base, size, type, physicalAddressBits,
                           &range->rawBase, &range->rawMask) !=
        MTRR_REQUEST_VALID)
        return 0;
    plan->rangeCount++;
    return 1;
}

static void
sortPlannedRanges(MTRRRepackPlan *plan)
{
    MTRRPlannedRange saved;
    unsigned index;
    unsigned position;

    for (index = 1; index < plan->rangeCount; index++) {
        saved = plan->ranges[index];
        position = index;
        while (position > 0 &&
               plan->ranges[position - 1].base > saved.base) {
            plan->ranges[position] = plan->ranges[position - 1];
            position--;
        }
        plan->ranges[position] = saved;
    }
}

static int
verifyUCRepack(const MTRRRange *source,
               MTRRU64 requestBase,
               MTRRU64 requestSize,
               unsigned physicalAddressBits,
               const MTRRRepackPlan *plan)
{
    MTRRU64 requestEnd;
    MTRRU64 expectedBase;
    MTRRU64 expectedRawBase;
    MTRRU64 expectedRawMask;
    unsigned index;
    unsigned wcCount;

    if (source == 0 || plan == 0 || source->status != MTRR_RANGE_VALID ||
        source->type != MTRR_TYPE_UC || plan->rangeCount == 0 ||
        plan->rangeCount > MTRR_REPACK_MAX_RANGES)
        return 0;
    requestEnd = requestBase + requestSize - 1ULL;
    if (requestEnd < requestBase)
        return 0;

    expectedBase = source->base;
    wcCount = 0;
    for (index = 0; index < plan->rangeCount; index++) {
        const MTRRPlannedRange *range;

        range = &plan->ranges[index];
        if (range->base != expectedBase || range->size < 0x1000ULL ||
            (range->size & (range->size - 1ULL)) != 0 ||
            (range->base & (range->size - 1ULL)) != 0 ||
            range->end != range->base + range->size - 1ULL ||
            range->end > source->end)
            return 0;
        if (MTRRCoreBuildRange(range->base, range->size, range->type,
                               physicalAddressBits, &expectedRawBase,
                               &expectedRawMask) != MTRR_REQUEST_VALID ||
            range->rawBase != expectedRawBase ||
            range->rawMask != expectedRawMask)
            return 0;
        if (range->type == MTRR_TYPE_WC) {
            if (range->base != requestBase || range->size != requestSize ||
                range->end != requestEnd)
                return 0;
            wcCount++;
        } else if (range->type != MTRR_TYPE_UC) {
            return 0;
        }
        if (range->end == source->end) {
            if (index + 1U != plan->rangeCount)
                return 0;
        } else {
            expectedBase = range->end + 1ULL;
        }
    }
    return wcCount == 1U &&
           plan->ranges[plan->rangeCount - 1U].end == source->end;
}

MTRRRepackStatus
MTRRCorePlanUCRepack(const MTRRRange *source,
                     MTRRU64 requestBase,
                     MTRRU64 requestSize,
                     unsigned physicalAddressBits,
                     MTRRRepackPlan *plan)
{
    MTRRRequestStatus requestStatus;
    MTRRU64 requestEnd;
    MTRRU64 currentBase;
    MTRRU64 currentSize;
    MTRRU64 halfSize;

    if (plan == 0)
        return MTRR_REPACK_BUILD_FAILED;
    plan->rangeCount = 0;
    if (source == 0 || source->status != MTRR_RANGE_VALID)
        return MTRR_REPACK_SOURCE_INVALID;
    if (source->type != MTRR_TYPE_UC)
        return MTRR_REPACK_SOURCE_NOT_UC;
    requestStatus = MTRRCoreValidateRequest(requestBase, requestSize,
                                            physicalAddressBits,
                                            &requestEnd);
    if (requestStatus != MTRR_REQUEST_VALID)
        return MTRR_REPACK_REQUEST_INVALID;
    if (requestBase < source->base || requestEnd > source->end ||
        requestSize > source->size)
        return MTRR_REPACK_REQUEST_NOT_CONTAINED;

    currentBase = source->base;
    currentSize = source->size;
    while (currentSize > requestSize) {
        halfSize = currentSize >> 1;
        if (requestEnd < currentBase + halfSize) {
            if (!appendPlannedRange(plan, currentBase + halfSize, halfSize,
                                    MTRR_TYPE_UC, physicalAddressBits))
                return plan->rangeCount >= MTRR_REPACK_MAX_RANGES ?
                       MTRR_REPACK_TOO_MANY_RANGES :
                       MTRR_REPACK_BUILD_FAILED;
        } else if (requestBase >= currentBase + halfSize) {
            if (!appendPlannedRange(plan, currentBase, halfSize,
                                    MTRR_TYPE_UC, physicalAddressBits))
                return plan->rangeCount >= MTRR_REPACK_MAX_RANGES ?
                       MTRR_REPACK_TOO_MANY_RANGES :
                       MTRR_REPACK_BUILD_FAILED;
            currentBase += halfSize;
        } else {
            return MTRR_REPACK_REQUEST_NOT_CONTAINED;
        }
        currentSize = halfSize;
    }
    if (currentBase != requestBase || currentSize != requestSize)
        return MTRR_REPACK_REQUEST_NOT_CONTAINED;
    if (!appendPlannedRange(plan, requestBase, requestSize, MTRR_TYPE_WC,
                            physicalAddressBits))
        return plan->rangeCount >= MTRR_REPACK_MAX_RANGES ?
               MTRR_REPACK_TOO_MANY_RANGES : MTRR_REPACK_BUILD_FAILED;
    sortPlannedRanges(plan);
    if (!verifyUCRepack(source, requestBase, requestSize,
                        physicalAddressBits, plan))
        return MTRR_REPACK_VERIFICATION_FAILED;
    return MTRR_REPACK_VALID;
}

const char *
MTRRCoreRepackStatusName(MTRRRepackStatus status)
{
    switch (status) {
    case MTRR_REPACK_VALID:
        return "valid exact UC-to-WC partition";
    case MTRR_REPACK_SOURCE_INVALID:
        return "replacement source is not a valid MTRR range";
    case MTRR_REPACK_SOURCE_NOT_UC:
        return "replacement source is not UC";
    case MTRR_REPACK_REQUEST_INVALID:
        return "requested WC range is invalid";
    case MTRR_REPACK_REQUEST_NOT_CONTAINED:
        return "requested WC range is not exactly contained";
    case MTRR_REPACK_TOO_MANY_RANGES:
        return "partition exceeds the planner range limit";
    case MTRR_REPACK_BUILD_FAILED:
        return "partition contains an unrepresentable range";
    case MTRR_REPACK_VERIFICATION_FAILED:
        return "partition failed exact-coverage verification";
    default:
        return "unknown repacking error";
    }
}
