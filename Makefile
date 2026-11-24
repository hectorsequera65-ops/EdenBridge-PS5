.DEFAULT_GOAL := all

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
HOST_CC ?= cc
WARNINGS := -std=c11 -Wall -Wextra -Werror
SOURCES := src/http.c src/config.c
ELF := build/EdenBridge-PS5.elf
HOST := build/eden-bridge-local

ifneq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

.PHONY: all ps5 local test clean
all: ps5
ps5: $(ELF)
local: $(HOST)
test: local
	python3 smoke_test.py
	python3 native_test.py

$(ELF): $(SOURCES) src/bridge.h Makefile
	@test -x "$(PS5_PAYLOAD_SDK)/bin/prospero-clang" || (echo "Falta el SDK: define PS5_PAYLOAD_SDK" >&2; exit 1)
	mkdir -p $(@D)
	"$(PS5_PAYLOAD_SDK)/bin/prospero-clang" $(WARNINGS) -O2 -DEDEN_PS5 $(SOURCES) -o $@

$(HOST): $(SOURCES) src/bridge.h Makefile
	mkdir -p $(@D)
	$(HOST_CC) $(WARNINGS) -O2 -D_DEFAULT_SOURCE $(SOURCES) -o $@

clean:
	rm -rf build
