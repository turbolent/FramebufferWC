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
    if (!physicalAddressMask(physicalAddressBits, &addressMask))
        return MTRR_REQUEST_OUTSIDE_ADDRESS_WIDTH;
    if (rawBase != 0)
        *rawBase = (base & addressMask) | (type & 0xffU);
    if (rawMask != 0)
        *rawMask = ((~(size - 1ULL)) & addressMask) |
                   MTRR_PHYSMASK_VALID;
    return MTRR_REQUEST_VALID;
}

static int
knownMemoryType(unsigned type)
{
    return type == MTRR_TYPE_UC || type == MTRR_TYPE_WC ||
           type == MTRR_TYPE_WT || type == MTRR_TYPE_WP ||
           type == MTRR_TYPE_WB;
}

static int
wcCoversRequest(const MTRRRange *ranges, unsigned count,
                MTRRU64 base, MTRRU64 end)
{
    MTRRU64 coveredEnd;
    unsigned index;

    while (base <= end) {
        coveredEnd = base;
        for (index = 0; index < count; index++) {
            if (ranges[index].status == MTRR_RANGE_VALID &&
                ranges[index].type == MTRR_TYPE_WC &&
                ranges[index].base <= base && ranges[index].end >= base &&
                ranges[index].end + 1ULL > coveredEnd)
                coveredEnd = ranges[index].end + 1ULL;
        }
        if (coveredEnd == base)
            return 0;
        base = coveredEnd;
    }
    return 1;
}

static int
placeWCRange(MTRRWCPlan *plan, const unsigned *slots, unsigned slotCount,
             unsigned *used, MTRRU64 base, MTRRU64 size, unsigned type,
             unsigned physicalAddressBits)
{
    MTRRU64 addressMask;
    MTRRU64 rawBase;
    MTRRU64 rawMask;

    /* Count the whole partition even when it exceeds the available slots. */
    plan->requiredCount++;
    if (*used >= slotCount)
        return 1;
    if (!physicalAddressMask(physicalAddressBits, &addressMask))
        return 0;
    rawBase = base | type;
    rawMask = (~(size - 1ULL) & addressMask) | MTRR_PHYSMASK_VALID;
    return MTRRCoreDecodeRange(rawBase, rawMask, physicalAddressBits,
                               &plan->ranges[slots[(*used)++]]) == MTRR_RANGE_VALID;
}

/* Each endpoint starts a constant-coverage segment. Compare the set of types
 * on every segment, including uncovered memory and addresses above 4 GiB.
 * Equal sets preserve all overlap precedence outside the framebuffer.
 */
static unsigned
typesAt(const MTRRRange *ranges, unsigned count, MTRRU64 address,
         MTRRU64 *next)
{
    unsigned index;
    unsigned types;

    types = 0;
    for (index = 0; index < count; index++) {
        if (ranges[index].status != MTRR_RANGE_VALID)
            continue;
        if (ranges[index].base <= address && ranges[index].end >= address)
            types |= 1U << ranges[index].type;
        if (ranges[index].base > address && ranges[index].base < *next)
            *next = ranges[index].base;
        if (ranges[index].end + 1ULL > address &&
            ranges[index].end + 1ULL < *next)
            *next = ranges[index].end + 1ULL;
    }
    return types;
}

static int
verifyWCPlan(const MTRRRange *before, unsigned count,
              MTRRU64 requestBase, MTRRU64 requestEnd,
              unsigned physicalAddressBits, const MTRRWCPlan *plan)
{
    MTRRU64 address;
    MTRRU64 next;
    MTRRU64 limit;
    unsigned oldTypes;
    unsigned newTypes;

    limit = 1ULL << physicalAddressBits;
    address = 0;
    while (address < limit) {
        next = limit;
        if (requestBase > address)
            next = requestBase;
        else if (requestEnd >= address)
            next = requestEnd + 1ULL;
        oldTypes = typesAt(before, count, address, &next);
        newTypes = typesAt(plan->ranges, count, address, &next);
        if (address >= requestBase && address <= requestEnd) {
            if (newTypes != (1U << MTRR_TYPE_WC))
                return 0;
        } else if (oldTypes != newTypes) {
            return 0;
        }
        address = next;
    }
    return 1;
}

MTRRWCStatus
MTRRCorePlanFramebufferWC(const MTRRRange *ranges, unsigned rangeCount,
                          MTRRU64 requestBase, MTRRU64 requestSize,
                          unsigned physicalAddressBits,
                          int fixedRangesEnabled, MTRRWCPlan *plan)
{
    MTRRRange decoded;
    MTRRU64 addressMask;
    MTRRU64 requestEnd;
    MTRRU64 base;
    MTRRU64 size;
    MTRRU64 half;
    unsigned slots[MTRR_REPACK_MAX_RANGES];
    unsigned slotCount;
    unsigned used;
    unsigned index;
    int conflict;

    if (plan == 0)
        return MTRR_WC_INVALID_LAYOUT;
    plan->changedCount = 0;
    plan->requiredCount = 0;
    if (MTRRCoreValidateRequest(requestBase, requestSize,
                                physicalAddressBits, &requestEnd) !=
        MTRR_REQUEST_VALID)
        return MTRR_WC_INVALID_REQUEST;
    /* Fixed MTRRs can override variable ranges below 1 MiB. */
    if (fixedRangesEnabled && requestBase < 0x100000ULL)
        return MTRR_WC_LOW_MEMORY;
    if (ranges == 0 || rangeCount == 0 ||
        rangeCount > MTRR_REPACK_MAX_RANGES || ranges == plan->ranges)
        return MTRR_WC_INVALID_LAYOUT;
    if (!physicalAddressMask(physicalAddressBits, &addressMask))
        return MTRR_WC_INVALID_REQUEST;
    slotCount = 0;
    for (index = 0; index < rangeCount; index++) {
        MTRRCoreDecodeRange(ranges[index].rawBase, ranges[index].rawMask,
                            physicalAddressBits, &decoded);
        if (decoded.status != ranges[index].status)
            return MTRR_WC_INVALID_LAYOUT;
        if (decoded.status != MTRR_RANGE_UNUSED &&
            (decoded.status != MTRR_RANGE_VALID ||
             !knownMemoryType(decoded.type) ||
             (decoded.rawBase & ~(addressMask | 0xffULL)) != 0 ||
             (decoded.rawMask & ~(addressMask | MTRR_PHYSMASK_VALID)) != 0 ||
             decoded.base != ranges[index].base ||
             decoded.size != ranges[index].size ||
             decoded.end != ranges[index].end ||
             decoded.type != ranges[index].type))
            return MTRR_WC_INVALID_LAYOUT;
        plan->ranges[index] = decoded;
        conflict = decoded.status == MTRR_RANGE_VALID &&
                   decoded.type != MTRR_TYPE_WC &&
                   MTRRCoreRangesOverlap(requestBase, requestEnd,
                                         decoded.base, decoded.end);
        if (conflict) {
            slots[slotCount++] = index;
            plan->ranges[index].rawMask &= ~MTRR_PHYSMASK_VALID;
            plan->ranges[index].status = MTRR_RANGE_UNUSED;
        } else if (decoded.status == MTRR_RANGE_VALID) {
            plan->requiredCount++;
        }
    }
    /* Reuse conflicting entries before consuming firmware's free entries. */
    for (index = 0; index < rangeCount; index++) {
        if (ranges[index].status == MTRR_RANGE_UNUSED)
            slots[slotCount++] = index;
    }
    used = 0;
    for (index = 0; index < rangeCount; index++) {
        const MTRRRange *source;

        source = &ranges[index];
        if (source->status != MTRR_RANGE_VALID ||
            source->type == MTRR_TYPE_WC ||
            !MTRRCoreRangesOverlap(requestBase, requestEnd,
                                   source->base, source->end))
            continue;
        /* Aligned power-of-two ranges that overlap are nested. A source
         * contained by the request is removed; a containing source is split
         * into siblings retaining its original type outside the request.
         * These siblings may be above 4 GiB even on the 32-bit OS.
         */
        base = source->base;
        size = source->size;
        while (size > requestSize) {
            half = size >> 1;
            if (requestEnd < base + half) {
                if (!placeWCRange(plan, slots, slotCount, &used,
                                   base + half, half, source->type,
                                   physicalAddressBits))
                    return MTRR_WC_VERIFICATION_FAILED;
            } else if (requestBase >= base + half) {
                if (!placeWCRange(plan, slots, slotCount, &used,
                                   base, half, source->type, physicalAddressBits))
                    return MTRR_WC_VERIFICATION_FAILED;
                base += half;
            } else {
                return MTRR_WC_VERIFICATION_FAILED;
            }
            size = half;
        }
    }
    if (!wcCoversRequest(ranges, rangeCount, requestBase, requestEnd) &&
        !placeWCRange(plan, slots, slotCount, &used, requestBase, requestSize,
                      MTRR_TYPE_WC, physicalAddressBits))
        return MTRR_WC_VERIFICATION_FAILED;
    if (plan->requiredCount > rangeCount)
        return MTRR_WC_NO_SPACE;
    if (!verifyWCPlan(ranges, rangeCount, requestBase, requestEnd,
                       physicalAddressBits, plan))
        return MTRR_WC_VERIFICATION_FAILED;
    for (index = 0; index < rangeCount; index++) {
        if (plan->ranges[index].rawBase != ranges[index].rawBase ||
            plan->ranges[index].rawMask != ranges[index].rawMask)
            plan->changedCount++;
    }
    return plan->changedCount == 0 ? MTRR_WC_ALREADY_SET : MTRR_WC_VALID;
}

const char *
MTRRCoreWCStatusName(MTRRWCStatus status)
{
    switch (status) {
    case MTRR_WC_VALID:
        return "verified framebuffer WC partition";
    case MTRR_WC_ALREADY_SET:
        return "framebuffer is already write-combined";
    case MTRR_WC_INVALID_REQUEST:
        return "invalid framebuffer range";
    case MTRR_WC_INVALID_LAYOUT:
        return "invalid or unsupported firmware MTRR entry";
    case MTRR_WC_LOW_MEMORY:
        return "framebuffer is below 1 MiB and fixed MTRRs are enabled";
    case MTRR_WC_NO_SPACE:
        return "insufficient variable MTRRs for an exact partition";
    case MTRR_WC_VERIFICATION_FAILED:
        return "partition failed cache-type coverage verification";
    default:
        return "unknown framebuffer planning error";
    }
}
