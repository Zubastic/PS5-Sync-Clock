# PS5 Sync Clock - standalone PS5 payload build.

SHELL := /bin/sh

ifeq ($(strip $(PS5_PAYLOAD_SDK)),)
$(error PS5_PAYLOAD_SDK is required, e.g. export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk)
endif

CXX      := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
STRIP    := $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PYTHON   ?= python3

BIN := sync-clock.elf
BUILD_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

SRCS := src/main.cpp
SRCS += src/log.cpp
SRCS += src/websrv.cpp
SRCS += src/clock.cpp
SRCS += src/sntp.cpp
SRCS += src/asset.cpp
SRCS += src/api.cpp

ASSET := assets/index.html
GEN_SRC := gen/assets/index.html.cpp

OBJS := $(patsubst src/%.cpp,build/src/%.o,$(SRCS)) build/gen/assets/index.html.o

CXXFLAGS := -Os -Wall -Wextra -std=c++17 -Isrc
CXXFLAGS += -ffunction-sections -fdata-sections -flto
CXXFLAGS += -DSYNC_CLOCK_VERSION=\"$(BUILD_VERSION)\"
CXXFLAGS += -DSYNC_CLOCK_PORT=5915

LDFLAGS := -Wl,--gc-sections -flto
LDADD := -lSceNotification -lkernel

all: $(BIN)

gen/assets:
	mkdir -p $@

$(GEN_SRC): $(ASSET) | gen/assets
	$(PYTHON) gen-asset-module.py --path "" --mime "text/html" $< > $@

build/src/%.o: src/%.cpp Makefile
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/gen/%.o: gen/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BIN): $(OBJS) Makefile
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDADD)
	$(STRIP) --strip-all $@

clean:
	rm -rf build gen $(BIN)

.PHONY: all clean
