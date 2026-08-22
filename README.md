# FramebufferWC

FramebufferWC improves VBE framebuffer performance on OPENSTEP 4.2 x86.

It has two parts:

- The driver configures the framebuffer as write-combining with an MTRR.
- The VBE patch tells WindowServer to use a copy-back framebuffer mapping.

Both are needed for full WindowServer performance. The stock VBE driver sets
no cache-policy flag, so DriverKit defaults to a slow write-through mapping.
The patch preserves the gamma flag and adds `IO_DISPLAY_CACHE_COPYBACK`
for the active display and its mode list. It does not change the video mode,
framebuffer address, pixel format, or display hardware.

On a test machine, full-screen drawing improved from about 171 ms/frame
to 0.85-1.17 ms/frame.

## Build

```sh
cd /path/to/FramebufferWC
PATH=/usr/ucb:/bin:/usr/bin make clean all
```

The result is `FramebufferWC.config`. It includes the optional
`framebufferwc-control` and `vbe-cache-patch` utilities.

## Install

Copy `FramebufferWC.config` to `/private/Drivers/i386/` and make it owned by
`root.wheel`.

Add the driver immediately after VBE in
`/private/Drivers/i386/System.config/Instance0.table`:

```text
... VBE20DisplayDriver FramebufferWC ...
```

Check and patch the stock VBE driver:

```sh
/private/Drivers/i386/FramebufferWC.config/vbe-cache-patch status
su
/private/Drivers/i386/FramebufferWC.config/vbe-cache-patch patch
/usr/etc/reboot
```

The patcher accepts only the exact known stock OPENSTEP 4.2 VBE binary.
It creates and verifies `VBE20DisplayDriver_reloc.stock`, refuses to overwrite
an existing backup, installs through a temporary file, and verifies the result.
Unknown or partly modified binaries are refused.

A successful boot registers `Display0` followed by `FramebufferWC0`.

## Runtime control

```sh
/private/Drivers/i386/FramebufferWC.config/framebufferwc-control status
su
/private/Drivers/i386/FramebufferWC.config/framebufferwc-control off
/private/Drivers/i386/FramebufferWC.config/framebufferwc-control on
```

`off` restores the MTRR entries captured when the driver loaded. It does not
remove the VBE patch.

## Restore the stock VBE driver

```sh
su
/private/Drivers/i386/FramebufferWC.config/vbe-cache-patch restore
/usr/etc/reboot
```
