# =====================================================================
#  最小 UEFI 引导程序 + 内核
#
#    make          生成 build/EFI/BOOT/BOOTX64.EFI 和 build/kernel.elf
#    make run      用 QEMU + OVMF 启动（带图形窗口和串口输出）
#    make run-nox  无头模式运行，只从串口看输出
#    make test     在开发机上运行 host 单元测试（不开虚拟机）
#    make DEBUG=1  调试构建（-Og -g3，产物在 build/debug/），供 VS Code F5 使用
#    make esp      另外产出一个独立 FAT 镜像 dist/esp.img（真机 / 拷贝用）
#    make clean
# =====================================================================
#
#  build/ 目录本身就是“虚拟磁盘”。
#  QEMU 用 vvfat 把这个目录直接当成一块 FAT 硬盘挂给客户机：
#      -drive format=raw,file=fat:rw:build/
#  所以不需要 dd + mformat + mcopy 去打包镜像，编译产物放对位置就能启动。
#  目录布局就是 UEFI 固件要找的样子：
#      build/EFI/BOOT/BOOTX64.EFI   UEFI 固定查找的启动文件路径
#      build/kernel.elf             引导程序读的文件
#  往启动盘里加文件 = 直接往 build/ 里放文件，改完下一轮启动就生效。
#
#  注：vvfat 必须用 rw（只读模式 QEMU 会报 "Block node is read-only"），
#  也就是说客户机对这块盘有写权限，理论上能往 build/ 写文件。
# =====================================================================

# 内核链接地址：写进 kernel.elf 的段落地址（也就是程序头里的 p_paddr）。
# 引导程序不再需要这个常量——它完全按 ELF 程序头装载。
KERNEL_LOAD_ADDR ?= 0x100000

# 调试构建：make DEBUG=1
#   产物放进 build/debug/，和默认构建完全隔离（改优化级别不需要 make clean）；
#   因为 BUILD 就是磁盘根，调试构建自然也自成一个磁盘，互不干扰。
#   -Og 让单步跟源码对得上，-g3 连宏定义也给 GDB；
#   -fno-omit-frame-pointer 保证调用栈能回溯。
ifeq ($(DEBUG),1)
BUILD      := build/debug
KERNEL_OPT := -Og -g3 -fno-omit-frame-pointer
else
BUILD      := build
KERNEL_OPT := -O2
endif

# 独立镜像的输出目录（不和磁盘目录混在一起，否则镜像会变成磁盘里的一个文件）
DIST := dist
CLANG   ?= clang
LLD     ?= lld-link
CC      := gcc
LD      ?= ld

# ---- UEFI 引导程序：clang 生成 COFF，lld-link 链接成 UEFI 应用 --------
# 不用 gnu-efi，UEFI 的定义都在 boot/efi.h 里；
# ELF 的定义在 boot/elf.h，所以这里不需要 -DKERNEL_LOAD_ADDR
EFI_CFLAGS := --target=x86_64-unknown-windows \
              -std=c11 -O2 -Wall -Wextra \
              -ffreestanding -fno-builtin -fno-stack-protector \
              -mno-stack-arg-probe -mno-red-zone \
              -fno-asynchronous-unwind-tables

EFI_LDFLAGS := /nologo /subsystem:efi_application /entry:efi_main /nodefaultlib

# ---- 内核：直接产出 ELF，由引导程序按程序头装载 ----------------------
KCFLAGS := -std=c11 $(KERNEL_OPT) -Wall -Wextra \
           -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only \
           -fno-asynchronous-unwind-tables \
           -ffunction-sections -fdata-sections

# max-page-size 默认可能是 2MB，会让段落之间白空一大块、镜像变大；
# 内核只用 4KB 页，显式定死让地址紧凑、可预测。
# （段的划分由 kernel.lds 里的 PHDRS 决定，不由链接器选项决定。）
KERNEL_LDFLAGS := -nostdlib -T kernel.lds \
                  -z max-page-size=0x1000 \
                  --defsym=_kernel_load_addr=$(KERNEL_LOAD_ADDR) \
                  --no-warn-rwx-segments

# ---- QEMU / OVMF -----------------------------------------------------
QEMU      ?= qemu-system-x86_64
QEMU_MEM  ?= 256M
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd

ESP_MB := 64

MTOOLS := MTOOLS_SKIP_CHECK=1

# 调试用的 gdbstub 端口；run-gdb 只监听本机
GDB_PORT ?= 1234

# 调试时的显示方式；想边调试边看画面就用 QEMU_DISPLAY=gtk
QEMU_DISPLAY ?= none

# 引导程序的输出路径就是 UEFI 固定查找的路径；目录由规则自己 mkdir -p
BOOTX64  := $(BUILD)/EFI/BOOT/BOOTX64.EFI

all: $(BOOTX64) $(BUILD)/kernel.elf

# ---------------------------------------------------------------------
# 引导程序
# ---------------------------------------------------------------------
$(BUILD)/boot.o: boot/boot.c boot/efi.h boot/elf.h boot/bootinfo.h
	@mkdir -p $(BUILD)
	$(CLANG) $(EFI_CFLAGS) -c $< -o $@

$(BOOTX64): $(BUILD)/boot.o
	@mkdir -p $(dir $@)
	$(LLD) $(EFI_LDFLAGS) /out:$@ $<

# 内核目标文件
KERNEL_OBJS := $(BUILD)/main.o $(BUILD)/printf.o $(BUILD)/serial.o $(BUILD)/pmm.o $(BUILD)/text.o

.PHONY: all esp run run-nox run-gdb debug-stop test clean

# ---------------------------------------------------------------------
# 内核
# ---------------------------------------------------------------------
$(BUILD)/main.o: init/main.c boot/bootinfo.h lib/printf.h drivers/serial.h mm/pmm.h graphics/text.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/printf.o: lib/printf.c lib/printf.h drivers/serial.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/serial.o: drivers/serial.c drivers/serial.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/pmm.o: mm/pmm.c mm/pmm.h boot/bootinfo.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/text.o: graphics/text.c graphics/text.h graphics/console_font_a8.h boot/bootinfo.h
	@mkdir -p $(BUILD)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel.lds
	$(LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)

# ---------------------------------------------------------------------
# 独立的 FAT 镜像（给真机、拷贝给别人，或者不用 vvfat 的场合）
# 日常不需要它：QEMU 直接挂 $(BUILD)/ 目录就能启动。
# ---------------------------------------------------------------------
ESP_IMG := $(DIST)/esp.img

esp: $(ESP_IMG)

$(ESP_IMG): $(BOOTX64) $(BUILD)/kernel.elf
	@mkdir -p $(DIST)
	@rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=$(ESP_MB) status=none
	$(MTOOLS) mformat -i $@ -F ::
	$(MTOOLS) mmd   -i $@ ::/EFI
	$(MTOOLS) mmd   -i $@ ::/EFI/BOOT
	$(MTOOLS) mcopy -i $@ $(BOOTX64)          ::/EFI/BOOT/
	$(MTOOLS) mcopy -i $@ $(BUILD)/kernel.elf ::/
	@echo "==> $@ ($(ESP_MB) MiB，可 dd 到 U 盘或挂给别的虚拟机)"

# ---------------------------------------------------------------------
# 运行
# ---------------------------------------------------------------------
$(BUILD)/OVMF_VARS.fd: $(OVMF_VARS)
	@mkdir -p $(BUILD)
	cp $< $@

# 硬盘就是 $(BUILD)/ 目录本身：vvfat 把宿主机目录实时映射成一块 FAT 盘，
# 编译产物放进去就已经“在盘上”了，没有打包步骤。
# 注意必须用 rw：只读模式 QEMU 会直接报 "Block node is read-only" 起不来。
QEMU_DRIVES = \
	-drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
	-drive if=pflash,format=raw,unit=1,file=$(BUILD)/OVMF_VARS.fd \
	-drive format=raw,file=fat:rw:$(BUILD)/

# 串口：既显示在终端，也写一份到 $(SERIAL_LOG)。
# 这样调完能看到“输出到底停在哪一行”—— 调试时如果停在断点上，
# 日志就会在断点之前截断，是最直接的命中断点的证据。
SERIAL_LOG ?= $(BUILD)/serial.log
QEMU_SERIAL = -chardev stdio,id=vser,logfile=$(SERIAL_LOG) -serial chardev:vser

# 图形窗口里能看到 UEFI 控制台，内核的串口输出在终端里
run: all $(BUILD)/OVMF_VARS.fd
	$(QEMU) -m $(QEMU_MEM) $(QEMU_DRIVES) $(QEMU_SERIAL)

# 无头运行（服务器/SSH 下用这个）
run-nox: all $(BUILD)/OVMF_VARS.fd
	$(QEMU) -m $(QEMU_MEM) -display none $(QEMU_DRIVES) $(QEMU_SERIAL)

# 调试用：启动 QEMU 但把 CPU 停在复位向量（-S），等 GDB 从 gdbstub 连上来。
# VS Code 按 F5 时由 tools/qemu-debug.sh 调用；手动调试也可以直接跑这个目标，
# 然后另开终端：gdb build/debug/kernel.elf -ex 'target remote :1234'
run-gdb: all $(BUILD)/OVMF_VARS.fd
	@rm -f $(SERIAL_LOG)
	$(QEMU) -m $(QEMU_MEM) -display $(QEMU_DISPLAY) $(QEMU_DRIVES) $(QEMU_SERIAL) \
	        -gdb tcp::$(GDB_PORT) -S

# 停掉残留的调试用 QEMU（F5 会话结束后也会自动兜底）
# 模式里的 [g] 是防 pkill 匹配到自己所在 shell 的老办法
debug-stop:
	@pkill -f "[g]db tcp::$(GDB_PORT)" || true

# ---------------------------------------------------------------------
# printf 测试：把 lib/printf.c 拿到开发机上跑，不需要启动虚拟机
#   printf.c 只依赖一个 serial_putc()，测试里把它重定向到 stdout；
#   -Wno-format-truncation 是因为测试故意验证 snprintf 的截断行为
# ---------------------------------------------------------------------
HOSTCC ?= gcc
TEST_CFLAGS := -std=c11 -O2 -Wall -Wextra -fno-builtin -Wno-format-truncation

test: $(BUILD)/printf_test $(BUILD)/pmm_test $(BUILD)/text_test
	@./$(BUILD)/printf_test
	@./$(BUILD)/pmm_test
	@./$(BUILD)/text_test

$(BUILD)/printf_test: tests/printf_test.c lib/printf.c lib/printf.h
	@mkdir -p $(BUILD)
	$(HOSTCC) $(TEST_CFLAGS) tests/printf_test.c lib/printf.c -o $@

$(BUILD)/pmm_test: tests/pmm_test.c mm/pmm.c mm/pmm.h boot/bootinfo.h
	@mkdir -p $(BUILD)
	$(HOSTCC) $(TEST_CFLAGS) tests/pmm_test.c mm/pmm.c -o $@

$(BUILD)/text_test: tests/text_test.c graphics/text.c graphics/text.h graphics/console_font_a8.h boot/bootinfo.h
	@mkdir -p $(BUILD)
	$(HOSTCC) $(TEST_CFLAGS) tests/text_test.c graphics/text.c -o $@

clean:
	rm -rf $(BUILD) $(DIST)
