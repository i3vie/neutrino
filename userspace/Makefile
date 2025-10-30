# Userspace build helpers for Neutrino

SHELL := /bin/bash

CXX      ?= g++
NASM     ?= nasm
SUDO     ?= sudo

CRT_DIR  := crt
OBJ_DIR  := build
PROG_DIR := programs

# default program to build/install (override via `make PROGRAM=foo`)
PROGRAM        ?= shell
PROGRAM_SRC    := $(PROG_DIR)/$(PROGRAM).cpp
PROGRAM_OUTPUT := $(PROG_DIR)/$(PROGRAM).elf

# shared tool flags
ASFLAGS  := -f elf64
PROGRAM_CXXFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
                    -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f \
                    -fPIE -pie -Wall -Wextra -I$(CRT_DIR)

# deployment configuration (override as needed)
HDD_IMAGE ?= ../hdd.img
MOUNT_DIR ?= $(HOME)/MountedImage
INIT_ELF  ?= init.elf

.PHONY: all clean install-init

all: $(PROGRAM_OUTPUT)

$(OBJ_DIR)/crt0.o: $(CRT_DIR)/crt0.s $(CRT_DIR)/syscall.hpp | $(OBJ_DIR)
	$(NASM) $(ASFLAGS) $< -o $@

$(PROGRAM_OUTPUT): $(PROGRAM_SRC) $(OBJ_DIR)/crt0.o | $(PROG_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) $(OBJ_DIR)/crt0.o $(PROGRAM_SRC) -o $@

install-init: $(PROGRAM_OUTPUT)
	@set -euo pipefail; \
	$(SUDO) mkdir -p "$(MOUNT_DIR)"; \
	mounted=0; \
	cleanup() { if [[ $$mounted -eq 1 ]]; then $(SUDO) umount "$(MOUNT_DIR)"; fi; }; \
	trap cleanup EXIT INT TERM; \
	$(SUDO) mount -o loop,rw "$(HDD_IMAGE)" "$(MOUNT_DIR)"; \
	mounted=1; \
	$(SUDO) cp -f "$<" "$(MOUNT_DIR)/$(INIT_ELF)"; \
	sync

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(PROG_DIR):
	mkdir -p $(PROG_DIR)

clean:
	rm -rf $(OBJ_DIR) $(PROGRAM_OUTPUT)
