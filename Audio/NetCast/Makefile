NAME = NetCast
TYPE = SHARED

SRCS = \
	NetCastAddOn.cpp \
	NetCastNode.cpp \
	NetCastEncoder.cpp \
	NetCastServer.cpp

RDEFS = Resources.rdef

LIBS = be media network bnetapi mp3lame $(STDCPPLIBS)
OPTIMIZE := FULL
#DEFINES = HAVE_LAME=1 DEBUG=1 DEBUG_LEVEL=5
DEFINES = HAVE_LAME=1
WARNINGS = ALL
SYMBOLS := FALSE
COMPILER_FLAGS =
LINKER_FLAGS =

## Include the Makefile-Engine
DEVEL_DIRECTORY := \
	$(shell findpaths -r "makefile_engine" B_FIND_PATH_DEVELOP_DIRECTORY)
include $(DEVEL_DIRECTORY)/etc/makefile-engine
