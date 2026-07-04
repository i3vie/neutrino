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
HELPER_DIR := helpers
DEPS_DIR := deps
BEARSSL_DIR := $(DEPS_DIR)/BearSSL
OBJ_DIR  := build
SRC_PROG_DIR := programs
OUT_DIR  := out
BIN_DIR  := binary
LIB_DIR  := library
INCLUDE_DIR := include
CONFIG_DIR ?= config
DEPENDENCY_ROOT ?=
DEPENDENCY_INCLUDE_DIR ?= $(if $(DEPENDENCY_ROOT),$(DEPENDENCY_ROOT)/include,$(INCLUDE_DIR))
DEPENDENCY_LIBRARY_DIR ?= $(if $(DEPENDENCY_ROOT),$(DEPENDENCY_ROOT)/library,$(LIB_DIR))

# Every standalone source in programs/ is built and installed into /binary.
# Adding a new command therefore only requires programs/<name>.cpp.
DEFAULT_PROGRAMS := $(sort $(patsubst $(SRC_PROG_DIR)/%.cpp,%,$(wildcard $(SRC_PROG_DIR)/*.cpp)))
PROGRAMS ?= $(DEFAULT_PROGRAMS)
PROGRAM_OUTPUTS := $(addprefix $(OUT_DIR)/,$(addsuffix .elf,$(PROGRAMS)))

ASFLAGS  := -f elf64
PROGRAM_CXXFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
                    -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f \
                    -fPIE -pie -Wall -Wextra -I. -I$(CRT_DIR) -I$(LIBC_DIR)/include -I../shared/include \
                    -I$(DEPENDENCY_INCLUDE_DIR)
PROGRAM_LDFLAGS := -Wl,--no-dynamic-linker
SHARED_CXXFLAGS := $(filter-out -fPIE -pie,$(PROGRAM_CXXFLAGS)) -fPIC
PROGRAM_CFLAGS := -std=c99 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
                  -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f \
                  -fPIE -Wall -Wextra -I$(BEARSSL_DIR)/inc -I$(BEARSSL_DIR)/src \
                  -DBR_RDRAND=0 -DBR_USE_GETENTROPY=0 -DBR_USE_URANDOM=0 \
                  -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 \
                  -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0
SHARED_CFLAGS := $(filter-out -fPIE,$(PROGRAM_CFLAGS)) -fPIC
SHARED_LDFLAGS := -shared -nostdlib

BEARSSL_SOURCE_DIR := $(BEARSSL_DIR)/src
BEARSSL_SOURCE_INCLUDE_DIR := $(BEARSSL_DIR)/inc
BEARSSL_SOURCES := $(if $(wildcard $(BEARSSL_SOURCE_DIR)),$(shell cd $(BEARSSL_SOURCE_DIR) && find . -type f -name '*.c' | sed 's#^\./##' | sort))
BEARSSL_OBJECTS := $(addprefix $(OBJ_DIR)/bearssl/,$(BEARSSL_SOURCES:.c=.o))
BEARSSL_SHARED_OBJECTS := $(addprefix $(OBJ_DIR)/bearssl-pic/,$(BEARSSL_SOURCES:.c=.o))
BEARSSL_LIB := $(OBJ_DIR)/libbearssl.a
SHARED_LIBC_OBJECTS := $(OBJ_DIR)/shared/libc_string.o
BEARSSL_SO := $(OUT_DIR)/libbearssl.so.0
BEARSSL_SO_LINK := $(OUT_DIR)/libbearssl.so
BEARSSL_STAGED_SO := $(LIB_DIR)/libbearssl.so.0
BEARSSL_STAGED_SO_LINK := $(LIB_DIR)/libbearssl.so
BEARSSL_STAGED_HEADERS := $(addprefix $(INCLUDE_DIR)/,$(notdir $(wildcard $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl*.h)))
BEARSSL_DEP_SO := $(DEPENDENCY_LIBRARY_DIR)/libbearssl.so.0
BEARSSL_DEP_HEADERS := $(if $(DEPENDENCY_ROOT),$(DEPENDENCY_INCLUDE_DIR)/bearssl.h,$(BEARSSL_STAGED_HEADERS))

# Opt programs into BearSSL with:
# PROGRAM_LIBS_<program> += $(BEARSSL_DEP_SO)
PROGRAM_DEPS_browse += $(BEARSSL_DEP_HEADERS)
PROGRAM_DEPS_download += $(BEARSSL_DEP_HEADERS)
PROGRAM_DEPS_netget += $(BEARSSL_DEP_HEADERS)
PROGRAM_LIBS_browse += $(BEARSSL_DEP_SO)
PROGRAM_LIBS_download += $(BEARSSL_DEP_SO)
PROGRAM_LIBS_netget += $(BEARSSL_DEP_SO)
BEARSSL_PROGRAMS := browse download netget
SELECTED_BEARSSL_PROGRAMS := $(filter $(BEARSSL_PROGRAMS),$(PROGRAMS))
INSTALL_STAGED_LIBRARIES ?= $(if $(SELECTED_BEARSSL_PROGRAMS),1,0)
PROGRAM_HELPERS_browse += http
PROGRAM_HELPERS_installer += console
PROGRAM_HELPERS_lsdisk += console
PROGRAM_HELPERS_mount += args console
PROGRAM_HELPERS_mkneufs += args console
PROGRAM_HELPERS_mkpart += args console
PROGRAM_HELPERS_neupak += args text
PROGRAM_HELPERS_download += http
PROGRAM_HELPERS_netget += http
PROGRAM_DEPS_font += ../shared/include/TOSH-SAT.F14

HDD_IMAGE ?= ../hdd.img
MOUNT_DIR ?= $(HOME)/MountedImage
MOUNT_UID ?= $(shell id -u)
MOUNT_GID ?= $(shell id -g)
MOUNT_OPTS ?= loop,rw,uid=$(MOUNT_UID),gid=$(MOUNT_GID),umask=022
RESET_USERS ?= 0

.PHONY: all bearssl bearssl-shared clean install install-reset-users install-mount install-mtools
.SECONDEXPANSION:

ifneq ($(filter clean,$(MAKECMDGOALS)),)
.NOTPARALLEL:
endif

all: $(PROGRAM_OUTPUTS)

$(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h:
	@echo "BearSSL not found at $(BEARSSL_DIR)." >&2
	@echo "Provide your own BearSSL checkout there if you want BearSSL available in userspace." >&2
	@false

$(INCLUDE_DIR)/bearssl.h: $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h
	@mkdir -p $(INCLUDE_DIR)
	@cp $< $@

$(INCLUDE_DIR)/bearssl%.h: $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl%.h
	@mkdir -p $(INCLUDE_DIR)
	@cp $< $@

$(OBJ_DIR)/crt0.o: $(CRT_DIR)/crt0.s $(CRT_DIR)/syscall.hpp | $(OBJ_DIR)
	$(NASM) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/libc_string.o: $(LIBC_DIR)/string.cpp $(LIBC_DIR)/include/string.h | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/shared/libc_string.o: $(LIBC_DIR)/string.cpp $(LIBC_DIR)/include/string.h | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(SHARED_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/libc_ctype.o: $(LIBC_DIR)/ctype.cpp $(LIBC_DIR)/include/ctype.h | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/libc_neutrino.o: $(LIBC_DIR)/neutrino.cpp $(LIBC_DIR)/include/neutrino.h $(CRT_DIR)/syscall.hpp | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/helper_%.o: $(HELPER_DIR)/%.cpp $(HELPER_DIR)/%.hpp $(CRT_DIR)/syscall.hpp | $(OBJ_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/bearssl/%.o: $(BEARSSL_SOURCE_DIR)/%.c $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(PROGRAM_CFLAGS) -c $< -o $@

$(OBJ_DIR)/bearssl-pic/%.o: $(BEARSSL_SOURCE_DIR)/%.c $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CFLAGS) -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h $(BEARSSL_OBJECTS) | $(OBJ_DIR)
	$(AR) rcs $@ $(BEARSSL_OBJECTS)

$(BEARSSL_SO): $(BEARSSL_SOURCE_INCLUDE_DIR)/bearssl.h $(BEARSSL_SHARED_OBJECTS) $(SHARED_LIBC_OBJECTS) | $(OUT_DIR)
	$(CC) $(SHARED_LDFLAGS) -Wl,-soname,libbearssl.so.0 $(BEARSSL_SHARED_OBJECTS) $(SHARED_LIBC_OBJECTS) -o $@

$(BEARSSL_SO_LINK): $(BEARSSL_SO)
	@cp $(BEARSSL_SO) $@

$(BEARSSL_STAGED_SO): $(BEARSSL_SO)
	@mkdir -p $(LIB_DIR)
	@cp $(BEARSSL_SO) $@

$(BEARSSL_STAGED_SO_LINK): $(BEARSSL_SO)
	@mkdir -p $(LIB_DIR)
	@cp $(BEARSSL_SO) $@

bearssl: $(BEARSSL_LIB)
bearssl-shared: $(BEARSSL_SO) $(BEARSSL_SO_LINK) $(BEARSSL_STAGED_SO) $(BEARSSL_STAGED_SO_LINK) $(BEARSSL_STAGED_HEADERS)

INSTALL_PREREQS := $(if $(filter clean,$(MAKECMDGOALS)),clean) all $(if $(SELECTED_BEARSSL_PROGRAMS),bearssl-shared)
program_helper_objects = $(addprefix $(OBJ_DIR)/helper_,$(addsuffix .o,$(PROGRAM_HELPERS_$(1))))

$(OUT_DIR)/%.elf: $(SRC_PROG_DIR)/%.cpp $(OBJ_DIR)/crt0.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/libc_ctype.o $(OBJ_DIR)/libc_neutrino.o $(CRT_DIR)/syscall.hpp $$(PROGRAM_DEPS_$$*) $$(PROGRAM_LIBS_$$*) $$(call program_helper_objects,$$*) | $(OUT_DIR)
	@mkdir -p $(OUT_DIR) $(BIN_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) $(PROGRAM_LDFLAGS) $(OBJ_DIR)/crt0.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/libc_ctype.o $(OBJ_DIR)/libc_neutrino.o $(call program_helper_objects,$*) $< $(PROGRAM_LIBS_$*) -o $@
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
	$(SUDO) rm -rf "$(MOUNT_DIR)/library"; \
	$(SUDO) mkdir -p "$(MOUNT_DIR)/library"; \
	$(SUDO) rm -rf "$(MOUNT_DIR)/config"; \
	$(SUDO) mkdir -p "$(MOUNT_DIR)/config"; \
	if [[ "$(RESET_USERS)" == "1" ]]; then \
		$(SUDO) rm -f "$(MOUNT_DIR)/system/users.ntd" "$(MOUNT_DIR)/users.ntd"; \
	fi; \
	for prog in $(PROGRAMS); do \
		$(SUDO) install -m 0644 "$(OUT_DIR)/$$prog.elf" "$(MOUNT_DIR)/binary/$$prog.elf"; \
	done; \
	if [[ "$(INSTALL_STAGED_LIBRARIES)" == "1" && -d "$(LIB_DIR)" ]]; then \
		for lib in "$(LIB_DIR)"/*; do \
			if [[ -f "$$lib" ]]; then \
				$(SUDO) install -m 0644 "$$lib" "$(MOUNT_DIR)/library/$$(basename "$$lib")"; \
			fi; \
		done; \
	fi; \
	if [[ -d "$(CONFIG_DIR)" ]]; then \
		$(SUDO) cp -a "$(CONFIG_DIR)/." "$(MOUNT_DIR)/config/"; \
	fi; \
	if [[ -f "$(OUT_DIR)/init.elf" ]]; then \
		$(SUDO) install -m 0644 "$(OUT_DIR)/init.elf" "$(MOUNT_DIR)/init.elf"; \
	fi; \
	sync

install-reset-users:
	$(MAKE) install RESET_USERS=1

install-mtools: $(INSTALL_PREREQS)
	@set -euo pipefail; \
	mmd -i "$(HDD_IMAGE)" ::/binary ::/library ::/config; \
	for prog in $(PROGRAMS); do \
		mcopy -i "$(HDD_IMAGE)" "$(OUT_DIR)/$$prog.elf" "::/binary/$$prog.elf"; \
	done; \
	if [[ "$(INSTALL_STAGED_LIBRARIES)" == "1" && -d "$(LIB_DIR)" ]]; then \
		for lib in "$(LIB_DIR)"/*; do \
			if [[ -f "$$lib" ]]; then \
				mcopy -i "$(HDD_IMAGE)" "$$lib" "::/library/$$(basename "$$lib")"; \
			fi; \
		done; \
	fi; \
	if [[ -d "$(CONFIG_DIR)" ]]; then \
		mcopy -s -i "$(HDD_IMAGE)" "$(CONFIG_DIR)"/* ::/config/; \
	fi; \
	if [[ -f "$(OUT_DIR)/init.elf" ]]; then \
		mcopy -i "$(HDD_IMAGE)" "$(OUT_DIR)/init.elf" ::/init.elf; \
	fi

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

clean:
	rm -rf $(OBJ_DIR) $(OUT_DIR) $(BIN_DIR) $(LIB_DIR) $(INCLUDE_DIR)
