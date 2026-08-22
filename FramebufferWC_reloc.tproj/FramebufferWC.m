#define MACH_USER_API 1

#import <driverkit/generalFuncs.h>
#import <driverkit/kernelDriver.h>
#import <mach/machine.h>
#import "FramebufferWC.h"
#import "MTRRCore.h"
#import "MTRRX86.h"
#import <string.h>

#define FRAMEBUFFERWC_VERSION "0.27"
#define FRAMEBUFFERWC_MAX_VARIABLE_RANGES 32U
#define FRAMEBUFFERWC_MAX_VBE_MODES 256U
#define FRAMEBUFFERWC_VBE_DESCRIPTION_SIZE 512U
#define CPUID_FEATURE_MSR (1U << 5)
#define CPUID_FEATURE_MTRR (1U << 12)

extern machine_info_data_t machine_info;

/* One-shot pseudo-device workspace. Keep large arrays off the kernel stack. */
static MTRRRange savedRanges[FRAMEBUFFERWC_MAX_VARIABLE_RANGES];
static MTRRX86VariableRangeWrite writes[FRAMEBUFFERWC_MAX_VARIABLE_RANGES];
static MTRRX86VariableRangeWrite snapshots[FRAMEBUFFERWC_MAX_VARIABLE_RANGES];
static MTRRRepackPlan repackPlan;
static unsigned freeIndices[FRAMEBUFFERWC_MAX_VARIABLE_RANGES];
static MTRRU64 toggleRawDefault;
static unsigned toggleWriteCount;
static int toggleAvailable;
static FramebufferWC *registeredControl;
static unsigned char vbeDescription[FRAMEBUFFERWC_VBE_DESCRIPTION_SIZE];

static MTRRU32
high32(MTRRU64 value)
{
    return (MTRRU32)(value >> 32);
}

static MTRRU32
low32(MTRRU64 value)
{
    return (MTRRU32)value;
}

static int
parseUnsignedToken(const char *text, unsigned base, MTRRU64 *result)
{
    MTRRU64 value;
    unsigned digit;
    int sawDigit;

    value = 0;
    sawDigit = 0;
    while (*text != '\0' && *text != ' ' && *text != '\n' &&
           *text != '\r' && *text != ',') {
        if (*text >= '0' && *text <= '9')
            digit = (unsigned)(*text - '0');
        else if (*text >= 'a' && *text <= 'f')
            digit = (unsigned)(*text - 'a') + 10U;
        else if (*text >= 'A' && *text <= 'F')
            digit = (unsigned)(*text - 'A') + 10U;
        else
            return 0;
        if (digit >= base || value > (~0ULL - digit) / base)
            return 0;
        value = value * base + digit;
        sawDigit = 1;
        text++;
    }
    if (!sawDigit)
        return 0;
    *result = value;
    return 1;
}

static int
parseDescriptionField(const char *description, const char *field,
                      unsigned base, MTRRU64 *result)
{
    const char *value;

    value = strstr(description, field);
    if (value == 0)
        return 0;
    return parseUnsignedToken(value + strlen(field), base, result);
}

static int
readVBEString(id display, IOParameterName parameter)
{
    unsigned count;
    IOReturn status;

    count = sizeof(vbeDescription);
    vbeDescription[0] = '\0';
    vbeDescription[sizeof(vbeDescription) - 1U] = '\0';
    status = [display getCharValues:vbeDescription
                       forParameter:parameter
                              count:&count];
    vbeDescription[sizeof(vbeDescription) - 1U] = '\0';
    return status == IO_R_SUCCESS && count > 1U &&
           vbeDescription[0] != '\0';
}

static int
makeModeDescriptionParameter(char *buffer, unsigned capacity, unsigned mode)
{
    static const char prefix[] = "VBEModeDescription";
    char digits[10];
    unsigned digitCount;
    unsigned position;

    if (capacity <= sizeof(prefix))
        return 0;
    bcopy(prefix, buffer, sizeof(prefix) - 1U);
    digitCount = 0;
    do {
        digits[digitCount++] = (char)('0' + mode % 10U);
        mode /= 10U;
    } while (mode != 0U && digitCount < sizeof(digits));
    if (sizeof(prefix) + digitCount > capacity)
        return 0;
    position = sizeof(prefix) - 1U;
    while (digitCount != 0U)
        buffer[position++] = digits[--digitCount];
    buffer[position] = '\0';
    return 1;
}

static int
roundUpPowerOfTwo(MTRRU64 value, MTRRU64 *result)
{
    MTRRU64 rounded;

    rounded = 0x1000ULL;
    while (rounded < value) {
        if (rounded >= (1ULL << 32))
            return 0;
        rounded <<= 1;
    }
    *result = rounded;
    return 1;
}

static int
discoverVBEFramebuffer(MTRRU64 *framebufferBase, MTRRU64 *framebufferSize)
{
    id display;
    IOReturn status;
    MTRRU64 modeCountValue;
    MTRRU64 modeBase;
    MTRRU64 modeSize;
    MTRRU64 unavailableFlag;
    MTRRU64 discoveredBase;
    MTRRU64 largestMode;
    char parameter[32];
    unsigned modeCount;
    unsigned mode;
    unsigned usableModes;

    display = nil;
    status = IOGetObjectForDeviceName("Display0", &display);
    if (status != IO_R_SUCCESS || display == nil) {
        IOLog("FramebufferWC: VBE Display0 is not registered; no changes made\n");
        return 0;
    }
    if (!readVBEString(display, "VBEModeCount") ||
        !parseUnsignedToken((const char *)vbeDescription, 10U,
                            &modeCountValue) ||
        modeCountValue == 0 || modeCountValue > FRAMEBUFFERWC_MAX_VBE_MODES) {
        IOLog("FramebufferWC: Display0 does not expose a valid VBE mode table\n");
        return 0;
    }

    modeCount = (unsigned)modeCountValue;
    discoveredBase = 0;
    largestMode = 0;
    usableModes = 0;
    for (mode = 0; mode < modeCount; mode++) {
        if (!makeModeDescriptionParameter(parameter, sizeof(parameter), mode) ||
            !readVBEString(display, parameter) ||
            !parseDescriptionField((const char *)vbeDescription,
                                   "frameBuffer=", 16U, &modeBase) ||
            !parseDescriptionField((const char *)vbeDescription,
                                   "memorySize=", 10U, &modeSize) ||
            !parseDescriptionField((const char *)vbeDescription,
                                   "modeUnavailableFlag=", 16U,
                                   &unavailableFlag)) {
            IOLog("FramebufferWC: VBE mode %u has an invalid description; "
                  "no changes made\n", mode);
            return 0;
        }
        if (unavailableFlag != 0 || modeSize == 0)
            continue;
        if (modeBase == 0) {
            IOLog("FramebufferWC: VBE mode %u has no linear framebuffer; "
                  "no changes made\n", mode);
            return 0;
        }
        if (discoveredBase == 0)
            discoveredBase = modeBase;
        else if (modeBase != discoveredBase) {
            IOLog("FramebufferWC: VBE modes use different framebuffer bases; "
                  "no changes made\n");
            return 0;
        }
        if (modeSize > largestMode)
            largestMode = modeSize;
        usableModes++;
    }
    if (usableModes == 0 || discoveredBase == 0 || largestMode == 0 ||
        !roundUpPowerOfTwo(largestMode, framebufferSize) ||
        (discoveredBase & (*framebufferSize - 1ULL)) != 0) {
        IOLog("FramebufferWC: VBE framebuffer cannot be represented safely "
              "by one MTRR range\n");
        return 0;
    }
    *framebufferBase = discoveredBase;
    IOLog("FramebufferWC: VBE LFB base=0x%08x%08x size=0x%08x%08x "
          "(%u usable modes)\n",
          high32(*framebufferBase), low32(*framebufferBase),
          high32(*framebufferSize), low32(*framebufferSize), usableModes);
    return 1;
}

static int
variableWritesMatch(const MTRRX86VariableRangeWrite *expected,
                    unsigned writeCount,
                    const char *phase)
{
    MTRRU64 actualBase;
    MTRRU64 actualMask;
    unsigned position;
    int matched;

    matched = 1;
    for (position = 0; position < writeCount; position++) {
        actualBase = MTRRX86ReadMSR(MTRR_PHYSBASE0_MSR +
                                    expected[position].index * 2U);
        actualMask = MTRRX86ReadMSR(MTRR_PHYSMASK0_MSR +
                                    expected[position].index * 2U);
        if (actualBase != expected[position].rawBase ||
            actualMask != expected[position].rawMask) {
            matched = 0;
            if (phase == 0)
                return 0;
            IOLog("FramebufferWC: %s mismatch at MTRR #%u\n",
                  phase, expected[position].index);
            IOLog("FramebufferWC: expected base=0x%08x%08x "
                  "mask=0x%08x%08x\n",
                  high32(expected[position].rawBase),
                  low32(expected[position].rawBase),
                  high32(expected[position].rawMask),
                  low32(expected[position].rawMask));
            IOLog("FramebufferWC: actual   base=0x%08x%08x "
                  "mask=0x%08x%08x\n",
                  high32(actualBase), low32(actualBase),
                  high32(actualMask), low32(actualMask));
        }
    }
    return matched;
}

static FramebufferWCState
currentToggleState(void)
{
    if (!toggleAvailable)
        return FRAMEBUFFERWC_STATE_UNAVAILABLE;
    if (MTRRX86ReadMSR(MTRR_DEF_TYPE_MSR) != toggleRawDefault)
        return FRAMEBUFFERWC_STATE_INCONSISTENT;
    if (variableWritesMatch(writes, toggleWriteCount, 0))
        return FRAMEBUFFERWC_STATE_ON;
    if (variableWritesMatch(snapshots, toggleWriteCount, 0))
        return FRAMEBUFFERWC_STATE_OFF;
    return FRAMEBUFFERWC_STATE_INCONSISTENT;
}

static int
programAndVerify(const MTRRX86VariableRangeWrite *target,
                 const MTRRX86VariableRangeWrite *expectedCurrent,
                 unsigned writeCount,
                 MTRRU64 rawDefault)
{
    MTRRU64 currentDefault;

    currentDefault = MTRRX86ReadMSR(MTRR_DEF_TYPE_MSR);
    if (currentDefault != rawDefault) {
        IOLog("FramebufferWC: IA32_MTRR_DEF_TYPE changed after planning\n");
        IOLog("FramebufferWC: refusing update\n");
        return 0;
    }
    if (!variableWritesMatch(expectedCurrent, writeCount, "preflight")) {
        IOLog("FramebufferWC: MTRRs changed after planning; refusing update\n");
        return 0;
    }

    MTRRX86ProgramVariableRanges(target, writeCount, rawDefault);
    currentDefault = MTRRX86ReadMSR(MTRR_DEF_TYPE_MSR);
    if (currentDefault == rawDefault &&
        variableWritesMatch(target, writeCount, "readback")) {
        return 1;
    }
    if (currentDefault != rawDefault)
        IOLog("FramebufferWC: IA32_MTRR_DEF_TYPE readback mismatch\n");

    IOLog("FramebufferWC: readback failed; restoring preflight MTRR values\n");
    MTRRX86ProgramVariableRanges(expectedCurrent, writeCount, rawDefault);
    currentDefault = MTRRX86ReadMSR(MTRR_DEF_TYPE_MSR);
    if (currentDefault == rawDefault &&
        variableWritesMatch(expectedCurrent, writeCount, "rollback"))
        IOLog("FramebufferWC: rollback readback successful\n");
    else
        IOLog("FramebufferWC: CRITICAL: rollback readback failed; reboot now\n");
    return 0;
}

static BOOL
configureFramebufferWC(void)
{
    MTRRCPUIDResult basic;
    MTRRCPUIDResult features;
    MTRRCPUIDResult extended;
    MTRRCPUIDResult addressWidth;
    MTRRRange *range;
    MTRRRepackStatus repackStatus;
    MTRRU64 rawCapability;
    MTRRU64 rawDefault;
    MTRRU64 rawBase;
    MTRRU64 rawMask;
    MTRRU64 requestBase;
    MTRRU64 requestSize;
    MTRRU64 requestEnd;
    MTRRU64 candidateBase;
    MTRRU64 candidateMask;
    MTRRRequestStatus requestStatus;
    unsigned physicalAddressBits;
    unsigned index;
    unsigned variableCount;
    unsigned freeCount;
    unsigned freePosition;
    unsigned replaceIndex;
    unsigned containingUCCount;
    unsigned overlapCount;
    unsigned exactWCCount;
    unsigned plannedIndex;
    unsigned assignedIndex;
    unsigned writeCount;

    toggleAvailable = 0;
    toggleWriteCount = 0;
    toggleRawDefault = 0;
    IOLog("FramebufferWC %s: automatic VBE discovery\n",
          FRAMEBUFFERWC_VERSION);

    if (!MTRRX86CPUIDAvailable()) {
        IOLog("FramebufferWC: CPUID instruction is unavailable\n");
        return NO;
    }
    MTRRX86CPUID(0, 0, &basic);
    if (basic.eax < 1U) {
        IOLog("FramebufferWC: CPUID leaf 1 is unavailable\n");
        return NO;
    }
    MTRRX86CPUID(1, 0, &features);
    if ((features.edx & CPUID_FEATURE_MSR) == 0 ||
        (features.edx & CPUID_FEATURE_MTRR) == 0) {
        IOLog("FramebufferWC: required CPU features are unavailable\n");
        return NO;
    }

    physicalAddressBits = 0;
    MTRRX86CPUID(0x80000000U, 0, &extended);
    if (extended.eax >= 0x80000008U) {
        MTRRX86CPUID(0x80000008U, 0, &addressWidth);
        physicalAddressBits = addressWidth.eax & 0xffU;
    }
    if (physicalAddressBits < 12U || physicalAddressBits > 52U) {
        IOLog("FramebufferWC: physical address width is unavailable\n");
        return NO;
    }
    rawCapability = MTRRX86ReadMSR(MTRR_CAP_MSR);
    if ((rawCapability & MTRR_CAP_WRITE_COMBINING) == 0) {
        IOLog("FramebufferWC: CPU does not support WC variable MTRRs\n");
        return NO;
    }
    variableCount = (unsigned)(rawCapability &
                               MTRR_CAP_VARIABLE_COUNT_MASK);
    if (variableCount == 0 ||
        variableCount > FRAMEBUFFERWC_MAX_VARIABLE_RANGES) {
        IOLog("FramebufferWC: variable MTRR count is outside driver capacity\n");
        return NO;
    }
    rawDefault = MTRRX86ReadMSR(MTRR_DEF_TYPE_MSR);
    if ((rawDefault & MTRR_DEF_TYPE_ENABLE) == 0) {
        IOLog("FramebufferWC: MTRRs are disabled; no changes made\n");
        return NO;
    }
    if (machine_info.avail_cpus != 1) {
        IOLog("FramebufferWC: exactly one available processor is required; "
              "no changes made\n");
        return NO;
    }

    if (!discoverVBEFramebuffer(&requestBase, &requestSize))
        return NO;
    requestStatus = MTRRCoreValidateRequest(requestBase, requestSize,
                                            physicalAddressBits,
                                            &requestEnd);
    if (requestStatus != MTRR_REQUEST_VALID) {
        IOLog("FramebufferWC: discovered VBE range is invalid: %s\n",
              MTRRCoreRequestStatusName(requestStatus));
        return NO;
    }

    freeCount = 0;
    replaceIndex = variableCount;
    containingUCCount = 0;
    overlapCount = 0;
    exactWCCount = 0;
    for (index = 0; index < variableCount; index++) {
        rawBase = MTRRX86ReadMSR(MTRR_PHYSBASE0_MSR + index * 2U);
        rawMask = MTRRX86ReadMSR(MTRR_PHYSMASK0_MSR + index * 2U);
        range = &savedRanges[index];
        MTRRCoreDecodeRange(rawBase, rawMask, physicalAddressBits, range);
        if (range->status == MTRR_RANGE_UNUSED) {
            freeIndices[freeCount++] = index;
            continue;
        }
        if (range->status != MTRR_RANGE_VALID) {
            IOLog("FramebufferWC: MTRR #%u cannot be assessed safely; "
                  "no changes made\n", index);
            return NO;
        }
        if (!MTRRCoreRangesOverlap(requestBase, requestEnd,
                                   range->base, range->end))
            continue;
        overlapCount++;
        if (range->type == MTRR_TYPE_UC &&
            range->base <= requestBase && range->end >= requestEnd) {
            replaceIndex = index;
            containingUCCount++;
        }
        if (range->type == MTRR_TYPE_WC && range->base == requestBase &&
            range->size == requestSize)
            exactWCCount++;
    }

    if (exactWCCount == 1U && overlapCount == 1U) {
        IOLog("FramebufferWC: VBE LFB is already write-combined\n");
        return YES;
    }
    if (overlapCount != 0U &&
        (containingUCCount != 1U || overlapCount != 1U)) {
        IOLog("FramebufferWC: framebuffer MTRR layout is ambiguous; "
              "no changes made\n");
        return NO;
    }

    if (containingUCCount == 1U) {
        repackStatus = MTRRCorePlanUCRepack(&savedRanges[replaceIndex],
                                            requestBase, requestSize,
                                            physicalAddressBits,
                                            &repackPlan);
        if (repackStatus != MTRR_REPACK_VALID) {
            IOLog("FramebufferWC: automatic UC repack is unsafe: %s\n",
                  MTRRCoreRepackStatusName(repackStatus));
            return NO;
        }
        for (index = 0; index < variableCount; index++) {
            if (index == replaceIndex ||
                savedRanges[index].status == MTRR_RANGE_UNUSED)
                continue;
            if (MTRRCoreRangesOverlap(savedRanges[replaceIndex].base,
                                      savedRanges[replaceIndex].end,
                                      savedRanges[index].base,
                                      savedRanges[index].end)) {
                IOLog("FramebufferWC: containing UC range overlaps another "
                      "MTRR; no changes made\n");
                return NO;
            }
        }
        if (repackPlan.rangeCount > freeCount + 1U) {
            IOLog("FramebufferWC: automatic repack needs %u entries; only %u "
                  "are available\n", repackPlan.rangeCount, freeCount + 1U);
            return NO;
        }

        freePosition = 0;
        writeCount = 0;
        for (plannedIndex = 0; plannedIndex < repackPlan.rangeCount;
             plannedIndex++) {
            MTRRPlannedRange *planned;

            planned = &repackPlan.ranges[plannedIndex];
            assignedIndex = planned->type == MTRR_TYPE_WC ?
                            replaceIndex : freeIndices[freePosition++];
            writes[writeCount].index = assignedIndex;
            writes[writeCount].rawBase = planned->rawBase;
            writes[writeCount].rawMask = planned->rawMask;
            snapshots[writeCount].index = assignedIndex;
            snapshots[writeCount].rawBase = savedRanges[assignedIndex].rawBase;
            snapshots[writeCount].rawMask = savedRanges[assignedIndex].rawMask;
            writeCount++;
        }
        if (!programAndVerify(writes, snapshots, writeCount, rawDefault)) {
            IOLog("FramebufferWC: WC repack was not retained\n");
            return NO;
        }
    } else {
        if (freeCount == 0) {
            IOLog("FramebufferWC: no free variable MTRR is available\n");
            return NO;
        }
        requestStatus = MTRRCoreBuildRange(requestBase, requestSize,
                                           MTRR_TYPE_WC,
                                           physicalAddressBits,
                                           &candidateBase, &candidateMask);
        if (requestStatus != MTRR_REQUEST_VALID)
            return NO;
        writes[0].index = freeIndices[0];
        writes[0].rawBase = candidateBase;
        writes[0].rawMask = candidateMask;
        snapshots[0].index = freeIndices[0];
        snapshots[0].rawBase = savedRanges[freeIndices[0]].rawBase;
        snapshots[0].rawMask = savedRanges[freeIndices[0]].rawMask;
        writeCount = 1U;
        if (!programAndVerify(writes, snapshots, writeCount, rawDefault)) {
            IOLog("FramebufferWC: WC MTRR was not retained\n");
            return NO;
        }
    }

    toggleRawDefault = rawDefault;
    toggleWriteCount = writeCount;
    toggleAvailable = 1;
    IOLog("FramebufferWC: ready state=ON base=0x%08x%08x "
          "size=0x%08x%08x entries=%u\n",
          high32(requestBase), low32(requestBase),
          high32(requestSize), low32(requestSize), writeCount);
    return YES;
}

@implementation FramebufferWC

+ (IODeviceStyle)deviceStyle
{
    return IO_PseudoDevice;
}

+ (BOOL)probe:(IODeviceDescription *)deviceDescription
{
    FramebufferWC *driver;
    BOOL configured;

    if (registeredControl != nil) {
        IOLog("FramebufferWC: refusing a second control instance\n");
        return NO;
    }
    driver = [[self alloc] initFromDeviceDescription:deviceDescription];
    if (driver == nil)
        return NO;
    [driver setUnit:0];
    [driver setName:"FramebufferWC0"];
    [driver setDeviceKind:"MTRR Write-Combining Control"];

    [driver->_controlLock lock];
    if ([driver registerDevice] == nil) {
        [driver->_controlLock unlock];
        [driver free];
        return NO;
    }
    configured = configureFramebufferWC();
    [driver->_controlLock unlock];
    if (!configured) {
        [driver unregisterDevice];
        [driver free];
        return NO;
    }
    registeredControl = driver;
    return YES;
}

- initFromDeviceDescription:(IODeviceDescription *)deviceDescription
{
    if ([super initFromDeviceDescription:deviceDescription] == nil)
        return [super free];
    _controlLock = [[NXLock alloc] init];
    if (_controlLock == nil)
        return [super free];
    return self;
}

- free
{
    if (registeredControl == self)
        registeredControl = nil;
    if (_controlLock != nil) {
        [_controlLock free];
        _controlLock = nil;
    }
    return [super free];
}

- (IOReturn)getCharValues:(unsigned char *)values
             forParameter:(IOParameterName)parameterName
                     count:(unsigned *)count
{
    unsigned needed;

    if (strcmp(parameterName, FRAMEBUFFERWC_VERSION_PARAMETER) != 0)
        return [super getCharValues:values forParameter:parameterName
                              count:count];
    needed = strlen(FRAMEBUFFERWC_VERSION) + 1U;
    if (values == 0 || count == 0 || *count < needed) {
        if (count != 0)
            *count = needed;
        return IO_R_INVALID_ARG;
    }
    bcopy(FRAMEBUFFERWC_VERSION, values, needed);
    *count = needed;
    return IO_R_SUCCESS;
}

- (IOReturn)getIntValues:(unsigned *)values
            forParameter:(IOParameterName)parameterName
                   count:(unsigned *)count
{
    FramebufferWCState state;

    if (strcmp(parameterName, FRAMEBUFFERWC_STATUS_PARAMETER) != 0)
        return [super getIntValues:values forParameter:parameterName
                             count:count];
    if (values == 0 || count == 0 || *count < FRAMEBUFFERWC_STATUS_COUNT) {
        if (count != 0)
            *count = FRAMEBUFFERWC_STATUS_COUNT;
        return IO_R_INVALID_ARG;
    }

    [_controlLock lock];
    state = currentToggleState();
    values[FRAMEBUFFERWC_STATUS_SCHEMA] = FRAMEBUFFERWC_STATUS_SCHEMA_VERSION;
    values[FRAMEBUFFERWC_STATUS_READY] = toggleAvailable ? 1U : 0U;
    values[FRAMEBUFFERWC_STATUS_STATE] = (unsigned)state;
    values[FRAMEBUFFERWC_STATUS_WRITE_COUNT] = toggleWriteCount;
    [_controlLock unlock];
    *count = FRAMEBUFFERWC_STATUS_COUNT;
    return IO_R_SUCCESS;
}

- (IOReturn)setIntValues:(unsigned *)values
            forParameter:(IOParameterName)parameterName
                   count:(unsigned)count
{
    const MTRRX86VariableRangeWrite *target;
    const MTRRX86VariableRangeWrite *expectedCurrent;
    FramebufferWCState currentState;
    FramebufferWCState requestedState;
    int succeeded;
    if (strcmp(parameterName, FRAMEBUFFERWC_CONTROL_PARAMETER) != 0)
        return [super setIntValues:values forParameter:parameterName
                             count:count];
    if (values == 0 || count != 1U || values[0] > 1U)
        return IO_R_INVALID_ARG;

    [_controlLock lock];
    if (!toggleAvailable) {
        [_controlLock unlock];
        return IO_R_NOT_READY;
    }
    if (machine_info.avail_cpus != 1) {
        IOLog("FramebufferWC: runtime toggle refused: available CPU count is %d\n",
              machine_info.avail_cpus);
        [_controlLock unlock];
        return IO_R_UNSUPPORTED;
    }

    currentState = currentToggleState();
    if (currentState == FRAMEBUFFERWC_STATE_INCONSISTENT ||
        currentState == FRAMEBUFFERWC_STATE_UNAVAILABLE) {
        IOLog("FramebufferWC: runtime toggle refused: MTRR table is not an exact "
              "known state\n");
        [_controlLock unlock];
        return IO_R_IO;
    }
    requestedState = values[0] ? FRAMEBUFFERWC_STATE_ON : FRAMEBUFFERWC_STATE_OFF;
    if (requestedState == currentState) {
        IOLog("FramebufferWC: runtime toggle already %s; no change\n",
              requestedState == FRAMEBUFFERWC_STATE_ON ? "ON" : "OFF");
        [_controlLock unlock];
        return IO_R_SUCCESS;
    }

    if (requestedState == FRAMEBUFFERWC_STATE_ON) {
        target = writes;
        expectedCurrent = snapshots;
    } else {
        target = snapshots;
        expectedCurrent = writes;
    }
    IOLog("FramebufferWC: runtime toggle request %s -> %s\n",
          currentState == FRAMEBUFFERWC_STATE_ON ? "ON" : "OFF",
          requestedState == FRAMEBUFFERWC_STATE_ON ? "ON" : "OFF");
    succeeded = programAndVerify(target, expectedCurrent,
                                 toggleWriteCount, toggleRawDefault);
    if (succeeded)
        IOLog("FramebufferWC: runtime toggle is now %s\n",
              requestedState == FRAMEBUFFERWC_STATE_ON ? "ON" : "OFF");
    else
        IOLog("FramebufferWC: runtime toggle failed; requested state not retained\n");
    [_controlLock unlock];
    return succeeded ? IO_R_SUCCESS : IO_R_IO;
}

@end
