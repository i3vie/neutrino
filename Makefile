# === Build configuration ===
ARCH       := x86_64
CROSS      := x86_64-elf-
CC         := $(CROSS)g++
LD         := $(CROSS)ld
AS         := nasm -f elf64
OBJCOPY    := $(CROSS)objcopy

OUT_DIR    := out
BUILD_DIR  := build
SRC_DIR    := src

TARGET_ELF := $(OUT_DIR)/kernel.elf
TARGET_ISO := $(OUT_DIR)/neutrino.iso

CFLAGS     := -std=c++20 -g -ffreestanding -O2 -Wall -Wextra -m64 -mno-red-zone -mno-sse -mno-mmx -mno-avx -mno-avx512f -mno-sse2 -fno-exceptions -fno-rtti -mcmodel=kernel $(EXTRA_CFLAGS) -Iinclude -I$(SRC_DIR) -I$(SRC_DIR)/arch/$(ARCH)
LDFLAGS    := -T $(SRC_DIR)/linker.ld -nostdlib

# === QEMU configuration ===
QEMU ?= qemu-system-x86_64
QEMU_BIOS ?= /usr/share/edk2/x64/OVMF.4m.fd
QEMU_COMMON_ARGS := -m 1G -cdrom $(TARGET_ISO) -serial stdio \
		-smp 4 -bios $(QEMU_BIOS) \
		-drive file=hdd.img,format=raw,if=ide \
		-enable-kvm -display sdl
QEMU_DEBUG_ARGS := -d int \
		-monitor unix:./qemu-monitor-socket,server,nowait -no-shutdown -no-reboot
QEMU_DEBUG_WAIT_ARGS := -s -S

TARGET_ISO_RAMFS := $(OUT_DIR)/neutrino_ramfs.iso
ISO_ROOT_RAMFS := $(OUT_DIR)/iso_root_ramfs
RAMFS_TRUNCATE_SIZE ?= 64M

# === Source discovery ===
SRC_CPP := $(shell find $(SRC_DIR) -type f -name '*.cpp')
SRC_ASM := $(shell find $(SRC_DIR) -type f -name '*.S')
OBJ     := $(SRC_CPP:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o) \
           $(SRC_ASM:$(SRC_DIR)/%.S=$(BUILD_DIR)/%.o)

# === Default target ===
all: $(TARGET_ELF)

# === Object build rules ===
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "[C++] $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	@echo "[ASM]  $<"
	$(AS) $< -o $@

# === Link kernel ELF ===
$(TARGET_ELF): $(OBJ)
	@mkdir -p $(OUT_DIR)
	@echo "[LD]   $@"
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

# === Grab Limine if it's missing ===
LIMINE_DIR := limine

$(LIMINE_DIR):
	@echo "[Limine] Cloning Limine..."
	git clone https://codeberg.org/Limine/Limine.git $(LIMINE_DIR) --branch=v10.x-binary --depth=1
	cd limine
	make -C limine

# === Build bootable UEFI ISO ===
ISO_ROOT := $(OUT_DIR)/iso_root

$(TARGET_ISO): $(TARGET_ELF) $(LIMINE_DIR)
	@echo "[ISO] Building $@"

	mkdir -p $(ISO_ROOT)/boot
	cp -v out/kernel.elf $(ISO_ROOT)/boot/kernel.elf
	mkdir -p $(ISO_ROOT)/boot/limine
	cp -v $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	
	echo 'timeout: 1' > $(ISO_ROOT)/limine.conf
	echo '' >> $(ISO_ROOT)/limine.conf
	echo '/Neutrino' >> $(ISO_ROOT)/limine.conf
	echo '    protocol: limine' >> $(ISO_ROOT)/limine.conf
	echo '    path: boot():/boot/kernel.elf' >> $(ISO_ROOT)/limine.conf
	echo '    cmdline: ROOT=IDE_PM_0' >> $(ISO_ROOT)/limine.conf

	mkdir -p $(ISO_ROOT)/EFI/BOOT
	cp -v $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	cp -v $(LIMINE_DIR)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/

	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        $(ISO_ROOT) -o $(TARGET_ISO)
	
	$(LIMINE_DIR)/limine bios-install $(TARGET_ISO)

$(TARGET_ISO_RAMFS): $(TARGET_ELF) $(LIMINE_DIR) hdd.img
	@echo "[ISO] Building $@ (RAMFS)"

	rm -rf $(ISO_ROOT_RAMFS)
	mkdir -p $(ISO_ROOT_RAMFS)/boot
	cp -v out/kernel.elf $(ISO_ROOT_RAMFS)/boot/kernel.elf
	mkdir -p $(ISO_ROOT_RAMFS)/boot/limine
	cp -v $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT_RAMFS)/boot/limine/

	cp -v hdd.img $(ISO_ROOT_RAMFS)/boot/rootfs.img
	truncate -s $(RAMFS_TRUNCATE_SIZE) $(ISO_ROOT_RAMFS)/boot/rootfs.img

	printf 'timeout: 1\n\n' > $(ISO_ROOT_RAMFS)/limine.conf
	printf '/Neutrino\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    protocol: limine\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    path: boot():/boot/kernel.elf\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    cmdline: ROOT=IDE_PM_0\n\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '/Neutrino (rootfs in memory)\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    protocol: limine\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    path: boot():/boot/kernel.elf\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    cmdline: ROOT=MEMDISK_0_0\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    module_path: boot():/boot/rootfs.img\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '    module_cmdline: rootfs\n' >> $(ISO_ROOT_RAMFS)/limine.conf
	printf '\n' >> $(ISO_ROOT_RAMFS)/limine.conf

	mkdir -p $(ISO_ROOT_RAMFS)/EFI/BOOT
	cp -v $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT_RAMFS)/EFI/BOOT/
	cp -v $(LIMINE_DIR)/BOOTIA32.EFI $(ISO_ROOT_RAMFS)/EFI/BOOT/

	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        $(ISO_ROOT_RAMFS) -o $(TARGET_ISO_RAMFS)
	
	$(LIMINE_DIR)/limine bios-install $(TARGET_ISO_RAMFS)

run: $(TARGET_ISO)
	$(QEMU) $(QEMU_COMMON_ARGS)

# === Run but wait for debugger to attach ===
debug: $(TARGET_ISO)
	$(QEMU) $(QEMU_COMMON_ARGS) $(QEMU_DEBUG_ARGS) $(QEMU_DEBUG_WAIT_ARGS)

# === Run with -d int, do not wait for debugger to attach ===
debug-nostop: $(TARGET_ISO)
	$(QEMU) $(QEMU_COMMON_ARGS) $(QEMU_DEBUG_ARGS)

# === Utility targets ===
iso: $(TARGET_ISO)
	@echo ISO created at $(TARGET_ISO)

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)

iso-ramfs: $(TARGET_ISO_RAMFS)
	@echo ISO with RAMFS created at $(TARGET_ISO_RAMFS)

.PHONY: all clean iso iso-ramfs run
