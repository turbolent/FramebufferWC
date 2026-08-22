# OPENSTEP 4.2 DriverKit bundle.
NAME = FramebufferWC
PROJECTVERSION = 1.1
LANGUAGE = English
LOCAL_RESOURCES = Localizable.strings
GLOBAL_RESOURCES = Default.table framebufferwc-control vbe-cache-patch
TOOLS = FramebufferWC_reloc.tproj
OTHERSRCS = Makefile.preamble Makefile Makefile.postamble \
	framebufferwc-control.m vbe-cache-patch.c
MAKEFILEDIR = /NextDeveloper/Makefiles/app
MAKEFILE = bundle.make
SOURCEMODE = 444
BUNDLE_EXTENSION = config
-include Makefile.preamble
include $(MAKEFILEDIR)/$(MAKEFILE)
-include Makefile.postamble
-include Makefile.dependencies
