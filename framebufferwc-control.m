#import <driverkit/IODeviceMaster.h>
#import <driverkit/driverTypes.h>
#import <driverkit/return.h>
#import <stdio.h>
#import <string.h>
#import <unistd.h>
#import "FramebufferWCControl.h"

/* OPENSTEP 4.2's public unistd.h omits this BSD routine's prototype. */
extern int geteuid(void);

static const char *
stateName(unsigned state)
{
    switch (state) {
        case FRAMEBUFFERWC_STATE_OFF:
            return "OFF (firmware MTRRs restored)";
        case FRAMEBUFFERWC_STATE_ON:
            return "ON (write combining active)";
        case FRAMEBUFFERWC_STATE_INCONSISTENT:
            return "INCONSISTENT (toggle refused)";
        default:
            return "UNAVAILABLE";
    }
}

static int
usage(const char *program)
{
    fprintf(stderr, "usage: %s status|on|off\n", program);
    return 2;
}

int
main(int argc, char **argv)
{
    IODeviceMaster *master;
    IOObjectNumber objectNumber;
    IOString deviceKind;
    unsigned char version[32];
    unsigned status[FRAMEBUFFERWC_STATUS_COUNT];
    unsigned count;
    unsigned requested;
    IOReturn result;
    int changing;

    if (argc != 2)
        return usage(argv[0]);
    if (strcmp(argv[1], "status") == 0) {
        changing = 0;
        requested = 0;
    } else if (strcmp(argv[1], "on") == 0) {
        changing = 1;
        requested = 1;
    } else if (strcmp(argv[1], "off") == 0) {
        changing = 1;
        requested = 0;
    } else {
        return usage(argv[0]);
    }
    if (changing && geteuid() != 0) {
        fprintf(stderr, "ON/OFF changes require root\n");
        return 1;
    }

    master = [IODeviceMaster new];
    if (master == nil) {
        fprintf(stderr, "cannot open DriverKit device master\n");
        return 1;
    }
    result = [master lookUpByDeviceName:"FramebufferWC0"
                              objectNumber:&objectNumber
                                deviceKind:&deviceKind];
    if (result != IO_R_SUCCESS) {
        fprintf(stderr, "FramebufferWC0 is not loaded (%d)\n", result);
        [master free];
        return 1;
    }

    if (changing) {
        result = [master setIntValues:&requested
                          forParameter:FRAMEBUFFERWC_CONTROL_PARAMETER
                          objectNumber:objectNumber
                                 count:1];
        if (result != IO_R_SUCCESS) {
            fprintf(stderr, "cannot switch framebuffer WC %s (%d)\n",
                    requested ? "ON" : "OFF", result);
            [master free];
            return 1;
        }
    }

    count = sizeof(version);
    result = [master getCharValues:version
                       forParameter:FRAMEBUFFERWC_VERSION_PARAMETER
                       objectNumber:objectNumber
                              count:&count];
    if (result != IO_R_SUCCESS) {
        fprintf(stderr, "driver version is unavailable (%d)\n", result);
        [master free];
        return 1;
    }

    count = FRAMEBUFFERWC_STATUS_COUNT;
    result = [master getIntValues:status
                      forParameter:FRAMEBUFFERWC_STATUS_PARAMETER
                      objectNumber:objectNumber
                             count:&count];
    if (result != IO_R_SUCCESS || count != FRAMEBUFFERWC_STATUS_COUNT ||
        status[FRAMEBUFFERWC_STATUS_SCHEMA] !=
            FRAMEBUFFERWC_STATUS_SCHEMA_VERSION) {
        fprintf(stderr, "driver status is unavailable or incompatible (%d)\n",
                result);
        [master free];
        return 1;
    }

    printf("FramebufferWC0: driver %s, toggle %s, state %s, "
           "%u managed entries\n",
           version,
           status[FRAMEBUFFERWC_STATUS_READY] ? "ready" : "not ready",
           stateName(status[FRAMEBUFFERWC_STATUS_STATE]),
           status[FRAMEBUFFERWC_STATUS_WRITE_COUNT]);
    [master free];
    return 0;
}
