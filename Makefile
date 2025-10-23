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

CFLAGS     := -std=c++20 -g -ffreestanding -O2 -Wall -Wextra -m64 -mno-red-zone -mno-sse -mno-mmx -mno-avx -mno-avx512f -mno-sse2 -fno-exceptions -fno-rtti -mcmodel=kernel $(EXTRA_CFLAGS) -I$(SRC_DIR) -I$(SRC_DIR)/arch/$(ARCH)
LDFLAGS    := -T $(SRC_DIR)/linker.ld -nostdlib

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

	mkdir -p $(ISO_ROOT)/EFI/BOOT
	cp -v $(LIMINE_DIR)/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	cp -v $(LIMINE_DIR)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/

	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        $(ISO_ROOT) -o $(TARGET_ISO)
	
	$(LIMINE_DIR)/limine bios-install $(TARGET_ISO)

run: $(TARGET_ISO)
	qemu-system-x86_64 -m 1G -bios /usr/share/edk2/x64/OVMF.4m.fd -cdrom $(TARGET_ISO) \
		-serial stdio -d int \
		-drive file=hdd.img,format=raw,if=ide

# === Run in QEMU (EFI mode) ===
run-nodisk: $(TARGET_ISO)
	qemu-system-x86_64 -m 1G -bios /usr/share/edk2/x64/OVMF.4m.fd -cdrom $(TARGET_ISO) -serial stdio -d int

# === Run but wait for debugger to attach ===
debug: $(TARGET_ISO)
	qemu-system-x86_64 -m 1G -bios /usr/share/edk2/x64/OVMF.4m.fd -cdrom $(TARGET_ISO) -serial stdio -d int -s -S -monitor unix:./qemu-monitor-socket,server,nowait -no-shutdown -no-reboot

# === Utility targets ===
iso: $(TARGET_ISO)

clean:
	rm -rf $(BUILD_DIR) $(OUT_DIR)

.PHONY: all clean iso run
