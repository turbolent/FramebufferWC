# OPENSTEP 4.2 DriverKit relocatable driver project.
NAME = FramebufferWC_reloc
PROJECTVERSION = 1.1
LANGUAGE = English
CLASSES = FramebufferWC.m
HFILES = FramebufferWC.h FramebufferWCControl.h MTRRCore.h MTRRX86.h
CFILES = MTRRCore.c MTRRX86.c
OTHERSRCS = Makefile.preamble Makefile \
            Makefile.driver_preamble Load_Commands.sect
MAKEFILEDIR = /NextDeveloper/Makefiles/app
MAKEFILE = tool.make
SOURCEMODE = 444
LIBS =
DEBUG_LIBS = $(LIBS)
PROF_LIBS = $(LIBS)
-include Makefile.preamble
include $(MAKEFILEDIR)/$(MAKEFILE)
-include Makefile.dependencies
