NAME = UVC
TYPE = SHARED
SRCS = \
	AddOn.cpp \
	Producer.cpp \
	libuvc/init.c \
	libuvc/misc.c \
	libuvc/stream.c \
	libuvc/frame.c \
	libuvc/diag.c \
	libuvc/device.c \
	libuvc/ctrl.c \
	libuvc/ctrl-gen.c

SYSTEM_INCLUDE_PATHS = \
	./ \
	./libuvc \
	/system/develop/headers/libusb-1.0

LIBS = media be $(STDCPPLIBS) usb-1.0 jpeg
OPTIMIZE := FULL
WARNINGS = NONE
#DEBUGGER := TRUE
#SYMBOLS := TRUE

DEVEL_DIRECTORY := \
	$(shell findpaths -r "makefile_engine" B_FIND_PATH_DEVELOP_DIRECTORY)
include $(DEVEL_DIRECTORY)/etc/makefile-engine
