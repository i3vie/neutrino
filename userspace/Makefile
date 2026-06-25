# Userspace build helpers for Neutrino

SHELL := /bin/bash

CXX      ?= g++
CC       ?= gcc
AR       ?= ar
NASM     ?= nasm
SUDO     ?=
MOUNT_SUDO ?= sudo

CRT_DIR  := crt
LIBC_DIR := libc
DEPS_DIR := deps
BEARSSL_DIR := $(DEPS_DIR)/BearSSL
OBJ_DIR  := build
SRC_PROG_DIR := programs
OUT_DIR  := out
BIN_DIR  := binary

# Every standalone source in programs/ is built and installed into /binary.
# Adding a new command therefore only requires programs/<name>.cpp.
PROGRAMS := $(sort $(patsubst $(SRC_PROG_DIR)/%.cpp,%,$(wildcard $(SRC_PROG_DIR)/*.cpp)))
PROGRAM_OUTPUTS := $(addprefix $(OUT_DIR)/,$(addsuffix .elf,$(PROGRAMS)))

ASFLAGS  := -f elf64
PROGRAM_CXXFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
                    -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f \
                    -fPIE -pie -Wall -Wextra -I. -I$(CRT_DIR) -I$(LIBC_DIR)/include -I../shared/include -I$(BEARSSL_DIR)/inc
PROGRAM_CFLAGS := -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
                  -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f \
                  -fPIE -Wall -Wextra -I$(BEARSSL_DIR)/inc -I$(BEARSSL_DIR)/src \
                  -DBR_RDRAND=0 -DBR_USE_GETENTROPY=0 -DBR_USE_URANDOM=0 \
                  -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 \
                  -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0

BEARSSL_SOURCES := $(shell cd $(BEARSSL_DIR)/src && find . -type f -name '*.c' | sed 's#^\./##' | sort)
BEARSSL_OBJECTS := $(addprefix $(OBJ_DIR)/bearssl/,$(BEARSSL_SOURCES:.c=.o))
BEARSSL_LIB := $(OBJ_DIR)/libbearssl.a

# Opt programs into BearSSL with:
# PROGRAM_LIBS_<program> += $(BEARSSL_LIB)
PROGRAM_LIBS_browse += $(BEARSSL_LIB)
PROGRAM_LIBS_download += $(BEARSSL_LIB)
PROGRAM_LIBS_netget += $(BEARSSL_LIB)
PROGRAM_DEPS_font += ../shared/include/TOSH-SAT.F14

HDD_IMAGE ?= ../hdd.img
MOUNT_DIR ?= $(HOME)/MountedImage
MOUNT_UID ?= $(shell id -u)
MOUNT_GID ?= $(shell id -g)
MOUNT_OPTS ?= loop,rw,uid=$(MOUNT_UID),gid=$(MOUNT_GID),umask=022
RESET_USERS ?= 0

.PHONY: all bearssl clean install install-reset-users install-mount install-mtools
.SECONDEXPANSION:

ifneq ($(filter clean,$(MAKECMDGOALS)),)
.NOTPARALLEL:
endif

all: $(PROGRAM_OUTPUTS)

$(BEARSSL_DIR)/inc/bearssl.h:
	@echo "BearSSL not found at $(BEARSSL_DIR)." >&2
	@echo "Provide your own BearSSL checkout there if you want BearSSL available in userspace." >&2
	@false

$(OBJ_DIR)/crt0.o: $(CRT_DIR)/crt0.s $(CRT_DIR)/syscall.hpp | $(OBJ_DIR)
	$(NASM) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/libc_string.o: $(LIBC_DIR)/string.cpp $(LIBC_DIR)/include/string.h | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/libc_ctype.o: $(LIBC_DIR)/ctype.cpp $(LIBC_DIR)/include/ctype.h | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/libc_neutrino.o: $(LIBC_DIR)/neutrino.cpp $(LIBC_DIR)/include/neutrino.h $(CRT_DIR)/syscall.hpp | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/bearssl/%.o: $(BEARSSL_DIR)/src/%.c $(BEARSSL_DIR)/inc/bearssl.h | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(PROGRAM_CFLAGS) -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_DIR)/inc/bearssl.h $(BEARSSL_OBJECTS) | $(OBJ_DIR)
	$(AR) rcs $@ $(BEARSSL_OBJECTS)

bearssl: $(BEARSSL_LIB)

INSTALL_PREREQS := $(if $(filter clean,$(MAKECMDGOALS)),clean) all

$(OUT_DIR)/%.elf: $(SRC_PROG_DIR)/%.cpp $(OBJ_DIR)/crt0.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/libc_ctype.o $(OBJ_DIR)/libc_neutrino.o $(CRT_DIR)/syscall.hpp $$(PROGRAM_DEPS_$$*) $$(PROGRAM_LIBS_$$*) | $(OUT_DIR)
	@mkdir -p $(OUT_DIR) $(BIN_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) $(OBJ_DIR)/crt0.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/libc_ctype.o $(OBJ_DIR)/libc_neutrino.o $< $(PROGRAM_LIBS_$*) -o $@
	@cp $@ $(BIN_DIR)/$*.elf

install: $(INSTALL_PREREQS)
	@set -euo pipefail; \
	mkdir -p "$(MOUNT_DIR)"; \
	mounted=0; \
	cleanup() { if [[ $$mounted -eq 1 ]]; then $(MOUNT_SUDO) umount "$(MOUNT_DIR)"; fi; }; \
	trap cleanup EXIT INT TERM; \
	$(MOUNT_SUDO) mount -o "$(MOUNT_OPTS)" "$(HDD_IMAGE)" "$(MOUNT_DIR)"; \
	mounted=1; \
	$(SUDO) rm -rf "$(MOUNT_DIR)/binary"; \
	$(SUDO) mkdir -p "$(MOUNT_DIR)/binary"; \
	$(SUDO) rm -rf "$(MOUNT_DIR)/config"; \
	$(SUDO) mkdir -p "$(MOUNT_DIR)/config"; \
	if [[ "$(RESET_USERS)" == "1" ]]; then \
		$(SUDO) rm -f "$(MOUNT_DIR)/system/users.ntd" "$(MOUNT_DIR)/users.ntd"; \
	fi; \
	for prog in $(PROGRAMS); do \
		$(SUDO) install -m 0644 "$(OUT_DIR)/$$prog.elf" "$(MOUNT_DIR)/binary/$$prog.elf"; \
	done; \
	if [[ -d "config" ]]; then \
		$(SUDO) cp -a "config/." "$(MOUNT_DIR)/config/"; \
	fi; \
	if [[ -f "$(OUT_DIR)/init.elf" ]]; then \
		$(SUDO) install -m 0644 "$(OUT_DIR)/init.elf" "$(MOUNT_DIR)/init.elf"; \
	fi; \
	sync

install-reset-users:
	$(MAKE) install RESET_USERS=1

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

clean:
	rm -rf $(OBJ_DIR) $(OUT_DIR) $(BIN_DIR)
