NAME = ScreenCapture
TYPE = SHARED
SRCS = AddOn.cpp Producer.cpp ScreenCapture.cpp
LIBS = media be game $(STDCPPLIBS)
OPTIMIZE := NONE
WARNINGS = NONE

DEVEL_DIRECTORY := \
	$(shell findpaths -r "makefile_engine" B_FIND_PATH_DEVELOP_DIRECTORY)
include $(DEVEL_DIRECTORY)/etc/makefile-engine
