# Build using the official Rack 2 SDK. Example:
# make RACK_DIR=/absolute/path/to/Rack-SDK -j2
RACK_DIR ?= ../Rack-SDK
SOURCES += $(wildcard src/*.cpp)
DISTRIBUTABLES += res LICENSE
include $(RACK_DIR)/plugin.mk
# Preserve finite-input checks and the original DSP math; do not use fast-math.
FLAGS += -fno-fast-math -ffp-contract=off -pthread
CXXFLAGS += -std=c++11
LDFLAGS += -pthread

ifdef ARCH_WIN
LDFLAGS += -static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
endif
