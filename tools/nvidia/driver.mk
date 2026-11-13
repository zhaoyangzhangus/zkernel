# 使用一个生产内核和 ESP；不构建阶段内核，也不使用预构建系统对象。
.PHONY: nvidia-deploy nvidia-driver nvidia-payload
.PHONY: vulkan-loader vulkan-deps vulkan-payload vulkan-deploy
vulkan-deploy:
	$(MAKE) -f GNUmakefile esp OS_NVIDIA=1 OS_VULKAN=1 OS_INTERACTIVE_DESKTOP=1

vulkan-payload: nvidia-payload vulkan-loader vulkan-deps
	python3 tools/nvidia/stage-vulkan.py --build "$(BUILD)" --cc "$(ELFCC)"

ifeq ($(OS_VULKAN),1)
ifneq ($(OS_NVIDIA),1)
$(error OS_VULKAN=1 requires OS_NVIDIA=1)
endif
esp: vulkan-payload
endif
vulkan-deps:
	python3 tools/nvidia/fetch-vulkan-deps.py --build "$(BUILD)"

vulkan-loader:
	python3 tools/nvidia/build-vulkan-loader.py --build "$(BUILD)"
nvidia-deploy:
	$(MAKE) -f GNUmakefile esp OS_NVIDIA=1 OS_INTERACTIVE_DESKTOP=1

ifeq ($(OS_NVIDIA),1)
NVIDIA_IRQ_MODE ?= msix
NVIDIA_DRIVER_OBJECT = $(BUILD)/nvidia/driver/driver.o
NVIDIA_RM_DRIVER_OBJECTS += $(NVIDIA_DRIVER_OBJECT)
KERNEL_OBJECT_FORMAT = elf64-x86-64
KERNEL_LINK = $(ELFCC) -nostdlib -no-pie -Wl,--build-id=none \
	-Wl,--gc-sections -Wl,-T,tools/nvidia/phase4-kernel.ld -Wl,-Map,$(BUILD)/kernel/kernel.map
# private 可避免 ELF 编译器配置泄漏到 PE 加载器/用户态目标。
$(BUILD)/kernel/%.o: private CC = $(ELFCC)
$(BUILD)/kernel/%.o: private OBJCOPY = $(ELFOBJCOPY)
$(BUILD)/kernel/%.o: private KERNEL_CFLAGS += -mcmodel=kernel -fno-pie -DOS_NVIDIA_PHASE5=1

$(NVIDIA_DRIVER_OBJECT): FORCE
	python3 tools/nvidia/build-driver.py --build "$(BUILD)" --cc "$(ELFCC)" --cxx "$(ELFCXX)" --ld "$(ELFLD)" --objcopy "$(ELFOBJCOPY)" --irq-mode "$(NVIDIA_IRQ_MODE)" --jobs 4

nvidia-driver: $(NVIDIA_DRIVER_OBJECT)
nvidia-payload:
	python3 tools/nvidia/stage-driver.py --build "$(BUILD)" --cc "$(ELFCC)"
esp: nvidia-payload
endif
