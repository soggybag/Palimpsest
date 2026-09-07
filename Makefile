# Palimpsest — live-edit looper firmware (Daisy Patch, legacy)
TARGET = Palimpsest

# Sources. Add src/*.cpp here as milestones land (M1a: src/LoopBuffer.cpp, ...).
CPP_SOURCES = Palimpsest.cpp

# Header search paths for the src/ tree.
C_INCLUDES += -I. -Isrc

# Library locations (relative to daisy/Palimpsest/).
LIBDAISY_DIR = ../../DaisyExamples/libDaisy
DAISYSP_DIR  = ../../DaisyExamples/DaisySP

OPT = -O2

# Core location and generic makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
