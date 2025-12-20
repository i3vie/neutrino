# Userspace build helpers for Neutrino

SHELL := /bin/bash

CXX      ?= g++
NASM     ?= nasm
SUDO     ?= sudo

CRT_DIR  := crt
OBJ_DIR  := build
PROG_DIR := programs
BIN_DIR  := binary

PROGRAMS := shell ls cat noop shm_parent shm_writer shm_reader
PROGRAM_OUTPUTS := $(addprefix $(PROG_DIR)/,$(addsuffix .elf,$(PROGRAMS)))

ASFLAGS  := -f elf64
PROGRAM_CXXFLAGS := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
                    -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-avx -mno-avx512f \
                    -fPIE -pie -Wall -Wextra -I$(CRT_DIR) -I../include

HDD_IMAGE ?= ../hdd.img
MOUNT_DIR ?= $(HOME)/MountedImage

.PHONY: all clean install

all: $(PROGRAM_OUTPUTS)

$(OBJ_DIR)/crt0.o: $(CRT_DIR)/crt0.s $(CRT_DIR)/syscall.hpp | $(OBJ_DIR)
	$(NASM) $(ASFLAGS) $< -o $@

$(PROG_DIR)/%.elf: $(PROG_DIR)/%.cpp $(OBJ_DIR)/crt0.o $(CRT_DIR)/syscall.hpp | $(PROG_DIR)
	$(CXX) $(PROGRAM_CXXFLAGS) $(OBJ_DIR)/crt0.o $< -o $@
	@mkdir -p $(BIN_DIR)
	@cp $@ $(BIN_DIR)/$*.elf

install: all
	@set -euo pipefail; \
	$(SUDO) mkdir -p "$(MOUNT_DIR)"; \
	mounted=0; \
	cleanup() { if [[ $$mounted -eq 1 ]]; then $(SUDO) umount "$(MOUNT_DIR)"; fi; }; \
	trap cleanup EXIT INT TERM; \
	$(SUDO) mount -o loop,rw "$(HDD_IMAGE)" "$(MOUNT_DIR)"; \
	mounted=1; \
	$(SUDO) rm -rf "$(MOUNT_DIR)/binary"; \
	$(SUDO) mkdir -p "$(MOUNT_DIR)/binary"; \
	for prog in $(PROGRAMS); do \
		$(SUDO) install -m 0644 "$(PROG_DIR)/$$prog.elf" "$(MOUNT_DIR)/binary/$$prog.elf"; \
	done; \
	if [[ -f "$(PROG_DIR)/shell.elf" ]]; then \
		$(SUDO) install -m 0644 "$(PROG_DIR)/shell.elf" "$(MOUNT_DIR)/init.elf"; \
	fi; \
	sync

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(PROG_DIR):
	mkdir -p $(PROG_DIR)

clean:
	rm -rf $(OBJ_DIR) $(PROGRAM_OUTPUTS) $(BIN_DIR)
