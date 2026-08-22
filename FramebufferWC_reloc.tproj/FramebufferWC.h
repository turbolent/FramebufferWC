#ifndef FRAMEBUFFERWC_DRIVER_H
#define FRAMEBUFFERWC_DRIVER_H

#import <driverkit/IODevice.h>
#import <machkit/NXLock.h>
#import "FramebufferWCControl.h"

@interface FramebufferWC : IODevice
{
@private
    NXLock *_controlLock;
}
+ (IODeviceStyle)deviceStyle;
+ (BOOL)probe:(IODeviceDescription *)deviceDescription;
- initFromDeviceDescription:(IODeviceDescription *)deviceDescription;
- free;
- (IOReturn)getCharValues:(unsigned char *)values
             forParameter:(IOParameterName)parameterName
                     count:(unsigned *)count;
- (IOReturn)getIntValues:(unsigned *)values
            forParameter:(IOParameterName)parameterName
                   count:(unsigned *)count;
- (IOReturn)setIntValues:(unsigned *)values
            forParameter:(IOParameterName)parameterName
                   count:(unsigned)count;
@end

#endif
