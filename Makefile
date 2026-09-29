BUILD := build/debug

CLANG ?= clang
LLD   ?= lld-link
CC    := gcc
LD    ?= ld

KERNEL_LOAD_ADDR ?= 0x100000

EFI_CFLAGS := --target=x86_64-unknown-windows -std=c11 -Og -g \
              -Wall -Wextra -ffreestanding -fno-builtin \
              -fno-stack-protector -mno-stack-arg-probe -mno-red-zone \
              -fno-asynchronous-unwind-tables
EFI_LDFLAGS := /nologo /subsystem:efi_application /entry:efi_main /nodefaultlib

KCFLAGS := -std=c11 -Og -g3 -fno-omit-frame-pointer -Wall -Wextra \
           -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only \
           -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
KERNEL_LDFLAGS := -nostdlib -T kernel.lds -z max-page-size=0x1000 \
                  --defsym=_kernel_load_addr=$(KERNEL_LOAD_ADDR) \
                  --no-warn-rwx-segments

QEMU      ?= qemu-system-x86_64
QEMU_MEM  ?= 256M
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
GDB_PORT  ?= 1234

BOOTX64 := $(BUILD)/EFI/BOOT/BOOTX64.EFI
KERNEL_OBJS := $(BUILD)/main.o $(BUILD)/printf.o $(BUILD)/pmm.o $(BUILD)/paging.o $(BUILD)/vm.o $(BUILD)/text.o

.PHONY: all run debug-stop clean

all: $(BOOTX64) $(BUILD)/kernel.elf

$(BUILD)/boot.o: boot/boot.c boot/efi.h boot/elf.h boot/bootinfo.h
	@mkdir -p $(BUILD)
	$(CLANG) $(EFI_CFLAGS) -c $< -o $@

$(BOOTX64): $(BUILD)/boot.o
	@mkdir -p $(dir $@)
	$(LLD) $(EFI_LDFLAGS) /out:$@ $<

$(BUILD)/main.o: init/main.c boot/bootinfo.h lib/printf.h mm/pmm.h mm/paging.h mm/vm.h graphics/text.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/printf.o: lib/printf.c lib/printf.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/pmm.o: mm/pmm.c mm/pmm.h boot/bootinfo.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/paging.o: mm/paging.c mm/paging.h mm/pmm.h mm/vm.h boot/bootinfo.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/vm.o: mm/vm.c mm/vm.h mm/pmm.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/text.o: graphics/text.c graphics/text.h graphics/console_font_a8.h boot/bootinfo.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel.lds
	$(LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)

$(BUILD)/OVMF_VARS.fd: $(OVMF_VARS)
	@mkdir -p $(BUILD)
	cp $< $@

QEMU_DRIVES = \
	-drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
	-drive if=pflash,format=raw,unit=1,file=$(BUILD)/OVMF_VARS.fd \
	-drive format=raw,file=fat:rw:$(BUILD)/
QEMU_TERM = -monitor none -serial none -debugcon stdio \
            -global isa-debugcon.iobase=0xe9

run: all $(BUILD)/OVMF_VARS.fd
	$(QEMU) -m $(QEMU_MEM) $(QEMU_DRIVES) $(QEMU_TERM) \
	        -gdb tcp::$(GDB_PORT) -S

debug-stop:
	@pkill -f "[g]db tcp::$(GDB_PORT)" || true

clean:
	rm -rf build
