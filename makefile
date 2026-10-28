TOOLPREFIX ?= x86_64-w64-mingw32-
CC = $(TOOLPREFIX)gcc
LD = $(TOOLPREFIX)gcc
AR = $(TOOLPREFIX)ar
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump
# These programs run in the build environment rather than in System.  Keep
# them on the native or WSL host so `make test` and build-id do not need Wine.
HOSTCC ?= gcc

# The original Windows build used a dedicated x86_64-elf toolchain for the
# dynamic ELF user images.  On Linux the native GNU ELF tools produce the same
# freestanding output, so prefer the dedicated toolchain when installed and
# otherwise use the distro target prefix (or the unprefixed tools as a last
# resort).  An explicit command-line/environment value always wins.
ifeq ($(origin ELFTOOLPREFIX),undefined)
ifeq ($(OS),Windows_NT)
ELFTOOLPREFIX = x86_64-elf-
else ifneq ($(shell command -v x86_64-elf-gcc >/dev/null 2>&1 && command -v x86_64-elf-g++ >/dev/null 2>&1 && echo yes),)
ELFTOOLPREFIX = x86_64-elf-
else ifneq ($(shell command -v x86_64-linux-gnu-gcc >/dev/null 2>&1 && command -v x86_64-linux-gnu-g++ >/dev/null 2>&1 && echo yes),)
ELFTOOLPREFIX = x86_64-linux-gnu-
else
ELFTOOLPREFIX =
endif
endif

DEBUG ?= 0
OS_DEBUG_SERIAL ?= 0
OS_REALTEST ?= 0
OS_REALTEST_FAILURE_TEST ?= 0
OS_ENABLE_FRAME_TRACE ?= 0
OS_PHASE7 ?= 0
OS_PHASE8 ?= 0
OS_PHASE9 ?= 0
OS_PHASE10 ?= 0
OS_PHASE13 ?= 0
OS_XHCI_DIAGNOSTIC_BOT ?= 0
OS_AUTOSTART_CHROME ?= 0
# Match run-qemu.sh: plain incremental builds must retain the normal desktop.
# Headless/self-test consumers can still opt out explicitly with =0.
OS_INTERACTIVE_DESKTOP ?= 1
OS_NVIDIA ?= $(if $(wildcard $(BUILD)/nvidia/driver/driver.o),1,0)
KERNEL_EXTRA_CFLAGS ?=
VM_FAULT_AROUND_PAGES ?= 16
QEMU_TEST_SECONDS ?= 60
QEMU_TEST_CPU ?= 4
.DEFAULT_GOAL := all
ifeq ($(DEBUG),1)
DEBUG_CFLAGS = -O0 -g3 -fno-omit-frame-pointer
else ifeq ($(DEBUG),2)
# Keep source-level symbols for the interactive debugger while retaining the
# release optimisation level. The graphical shell redraws the full framebuffer
# after pointer/key events, so -O1 makes input look like a stalled guest.
DEBUG_CFLAGS = -O2 -g3 -fno-omit-frame-pointer
else
DEBUG_CFLAGS = -O2
endif

COMMON_CFLAGS = $(DEBUG_CFLAGS) -Wall -Wextra -Werror -Wno-error=unused-function -ffreestanding -fno-builtin \
                -fno-stack-protector -fno-stack-check -fno-pic -mno-red-zone \
                -mno-sse -mno-mmx -mno-stack-arg-probe -fshort-wchar -Iinclude -MMD -MP \
                -DOS_REALTEST=$(OS_REALTEST)
KERNEL_CFLAGS = $(COMMON_CFLAGS) -mabi=sysv -DOS_KERNEL_BUILD \
                -DOS_DEBUG_SERIAL=$(OS_DEBUG_SERIAL) \
                -DOS_ENABLE_FRAME_TRACE=$(OS_ENABLE_FRAME_TRACE) \
                -DOS_PHASE7=$(OS_PHASE7) \
                -DOS_PHASE8=$(OS_PHASE8) \
                -DOS_PHASE9=$(OS_PHASE9) \
                -DOS_PHASE10=$(OS_PHASE10) \
                -DVM_FAULT_AROUND_PAGES=$(VM_FAULT_AROUND_PAGES) \
                -DOS_REALTEST_FAILURE_TEST=$(OS_REALTEST_FAILURE_TEST) \
                -DOS_XHCI_DIAGNOSTIC_BOT=$(OS_XHCI_DIAGNOSTIC_BOT) \
                -DOS_AUTOSTART_CHROME=$(OS_AUTOSTART_CHROME) \
                -DOS_INTERACTIVE_DESKTOP=$(OS_INTERACTIVE_DESKTOP) \
                $(KERNEL_EXTRA_CFLAGS)
USER_CFLAGS = $(COMMON_CFLAGS) -mabi=sysv -msse2 -Iuser/services/audiod \
              -Iuser/libc/include
USER_LIBC_SOURCES = user/libc/memory.c user/libc/syscall.c \
                    user/libc/personality.c \
                    user/libc/alloc.c user/libc/stdio.c user/libc/stdlib.c \
                    user/libc/random.c \
                    user/libc/string.c user/libc/ctype.c user/libc/env.c \
                    user/libc/exit.c user/libc/time.c user/libc/dirent.c \
                    user/libc/locale.c user/libc/socket.c user/libc/mman.c \
                    user/libc/process.c user/libc/pthread.c user/libc/uio.c \
                    user/libc/getopt.c user/libc/fnmatch.c user/libc/wchar.c \
                    user/libc/wctype.c user/libc/poll.c user/libc/select.c \
                    user/libc/netdb.c user/libc/glob.c \
                    user/libc/strings.c user/libc/libgen.c user/libc/signal.c \
                    user/libc/math.c user/libc/fenv.c user/libc/complex.c \
                    user/libc/uchar.c user/libc/threads.c user/libc/semaphore.c \
                    user/libc/posix_compat.c
USER_LIBC_ASM_SOURCES = user/libc/setjmp.S user/libc/signal_restorer.S
USER_LIBC_ASM_SOURCES += user/libc/posix_compat.S
# The freestanding CRT calls the application's main().  Keep that object out
# of both shared and static libraries: the shared object must not carry an
# unresolved application symbol, and legacy PE services do not define main().
DYNAMIC_LIBC_SOURCES = $(USER_LIBC_SOURCES)
USER_LIBC_OBJECTS = $(patsubst user/libc/%.c,$(BUILD)/user/libc-%.o,$(USER_LIBC_SOURCES))
USER_LIBC_ASM_OBJECTS = $(patsubst user/libc/%.S,$(BUILD)/user/libc-asm-%.o,$(USER_LIBC_ASM_SOURCES))
USER_LIBC_LINK_OBJECTS = $(USER_LIBC_OBJECTS) $(USER_LIBC_ASM_OBJECTS)
ELFTOOLPREFIX ?= x86_64-elf-
ELFCC = $(ELFTOOLPREFIX)gcc
ELFCXX = $(ELFTOOLPREFIX)g++
ELFLD = $(ELFTOOLPREFIX)ld
ELFOBJCOPY = $(ELFTOOLPREFIX)objcopy
ELFOBJDUMP = $(ELFTOOLPREFIX)objdump
ELF_COMMON_CFLAGS = $(DEBUG_CFLAGS) -Wall -Wextra -Werror -ffreestanding -fno-builtin \
                    -fno-stack-protector -fno-stack-check -fno-asynchronous-unwind-tables \
                    -fno-unwind-tables -mno-red-zone -mno-mmx -mabi=sysv \
                    -include user/runtime/elf_abi.h -Iinclude -Iuser/runtime -Iuser/libc/include \
                    -I$(BLEND2D_DIR) -DOS_REALTEST=$(OS_REALTEST) \
                    -DOS_DEBUG_SERIAL=$(OS_DEBUG_SERIAL) -MMD -MP
ELF_RUNTIME_CFLAGS = $(ELF_COMMON_CFLAGS) -fPIC
# Keep executable data references position independent as well.  Native
# Debian GCC emits R_X86_64_COPY for external data under -fPIE, while the
# freestanding System loader intentionally handles GOT/PLT relocations and
# does not provide a host ABI copy-relocation path.  -fPIC produces the same
# PIE-compatible code without making the image depend on that ABI detail.
ELF_APP_CFLAGS = $(ELF_COMMON_CFLAGS) -fPIC
ELF_LINK_FLAGS = --build-id=none
ELF_RUNTIME_DIR = $(BUILD)/elf
DYNAMIC_LIBC_OBJECTS = $(patsubst user/libc/%.c,$(ELF_RUNTIME_DIR)/libc-%.o,$(DYNAMIC_LIBC_SOURCES)) \
                       $(patsubst user/libc/%.S,$(ELF_RUNTIME_DIR)/libc-asm-%.o,$(USER_LIBC_ASM_SOURCES))
DYNAMIC_LOADER_ELF = $(ELF_RUNTIME_DIR)/ld-system.so.1
DYNAMIC_LIBC_ELF = $(ELF_RUNTIME_DIR)/libsystemc.so.1
DYNAMIC_GFX_LIBRARY_ELF = $(ELF_RUNTIME_DIR)/libgraphics.so.1
DYNAMIC_GFX_TEST_ELF = $(ELF_RUNTIME_DIR)/dyn-gfx.elf
DYNAMIC_LIBC_TEST_ELF = $(ELF_RUNTIME_DIR)/libc-test.elf
DYNAMIC_LINUX_EVENTS_TEST_ELF = $(ELF_RUNTIME_DIR)/linux-events-test.elf
DYNAMIC_FSYNC_PROBE_ELF = $(ELF_RUNTIME_DIR)/fat-fsync-probe.elf
DYNAMIC_IMAGEVIEW_ELF = $(ELF_RUNTIME_DIR)/imageview.elf
DYNAMIC_NASM_ELF = $(ELF_RUNTIME_DIR)/nasm.elf
DYNAMIC_NDISASM_ELF = $(ELF_RUNTIME_DIR)/ndisasm.elf
DYNAMIC_GSHELL_ELF = $(ELF_RUNTIME_DIR)/gshell.elf
DYNAMIC_CHROME_START_ELF = $(ELF_RUNTIME_DIR)/chrome-start.elf
DYNAMIC_NOTEPAD_ELF = $(ELF_RUNTIME_DIR)/notepad.elf
DYNAMIC_FILEMAN_ELF = $(ELF_RUNTIME_DIR)/fileman.elf
DYNAMIC_TASKMGR_ELF = $(ELF_RUNTIME_DIR)/taskmgr.elf
DYNAMIC_SETTINGS_ELF = $(ELF_RUNTIME_DIR)/settings.elf
LOADER_LDFLAGS = -nostdlib -Wl,--entry,efi_main -Wl,--subsystem,10 \
                 -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                 -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import
KERNEL_LDFLAGS = -nostdlib -Wl,--entry,kernel_entry -Wl,--subsystem,0 \
                 -Wl,--image-base,0xffffffff80000000 -Wl,--section-alignment,0x1000 \
                 -Wl,--file-alignment,0x1000 -Wl,--dynamicbase \
                 -Wl,-Map,$(BUILD)/kernel/kernel.map

BUILD ?= build
NASM_DIR = third_party/nasm
include tools/nasm-platform/sources.mk
NASM_SOURCES = $(addprefix $(NASM_DIR)/,$(NASM_SOURCE_FILES))
NASM_OBJECTS = $(patsubst $(NASM_DIR)/%.c,$(BUILD)/nasm/%.o,$(NASM_SOURCES))
NDISASM_SOURCES = $(addprefix $(NASM_DIR)/,$(NDISASM_SOURCE_FILES))
NDISASM_OBJECTS = $(patsubst $(NASM_DIR)/%.c,$(BUILD)/nasm/%.o,$(NDISASM_SOURCES))
NASM_DEPENDENCY_FILES = $(shell find $(BUILD)/nasm -type f -name '*.d' -print 2>/dev/null)
NASM_CFLAGS = $(filter-out -Werror,$(ELF_APP_CFLAGS)) \
	-DHAVE_CONFIG_H -D__unix__ \
	-Itools/nasm-platform -I$(NASM_DIR) -I$(NASM_DIR)/include \
	-I$(NASM_DIR)/x86 -I$(NASM_DIR)/asm -I$(NASM_DIR)/output \
	-I$(NASM_DIR)/zlib \
	-Wno-maybe-uninitialized -Wno-return-type -Wno-sign-compare
ifeq ($(OS),Windows_NT)
STAGE_LOG ?= $(BUILD)/qemu-native-serial.log
else
STAGE_LOG ?= $(BUILD)/qemu-serial.log
endif
DEBUG_SERIAL_STAMP = $(BUILD)/kernel/.debug-serial
DEBUG_SERIAL_CONFIG_VALUE = os-config-v1:$(OS_DEBUG_SERIAL):$(OS_REALTEST):$(OS_REALTEST_FAILURE_TEST):$(OS_XHCI_DIAGNOSTIC_BOT):$(OS_PHASE7):$(OS_PHASE8):$(OS_PHASE9):$(OS_PHASE10):$(OS_AUTOSTART_CHROME):$(OS_INTERACTIVE_DESKTOP):$(OS_NVIDIA)
DEBUG_SERIAL_STAMP_VALUE := $(shell cat '$(DEBUG_SERIAL_STAMP)' 2>/dev/null)
FRAME_TRACE_STAMP = $(BUILD)/kernel/.frame-trace
FRAME_TRACE_STAMP_VALUE := $(shell cat '$(FRAME_TRACE_STAMP)' 2>/dev/null)
FRAME_TRACE_KERNEL_OBJECTS = $(BUILD)/kernel/graphics-window.o \
                             $(BUILD)/kernel/syscall-graphics.o \
                             $(BUILD)/kernel/linux-abi.o
PHASE13_STAMP = $(BUILD)/kernel/.phase13
ifeq ($(OS_PHASE13),1)
PHASE13_KERNEL_OBJECT = $(BUILD)/kernel/linux-phase13-test.o
endif

# Every kernel object embeds the serial-debug switch.  Keep the stamp as a
# shared prerequisite so changing OS_DEBUG_SERIAL cannot leave a mixed
# image with only entry.o rebuilt.
KERNEL_DEBUG_STAMP_TARGETS = $(wildcard $(BUILD)/kernel/*.o)
$(KERNEL_DEBUG_STAMP_TARGETS): $(DEBUG_SERIAL_STAMP)
$(FRAME_TRACE_KERNEL_OBJECTS): $(FRAME_TRACE_STAMP)
ifneq ($(DEBUG_SERIAL_STAMP_VALUE),$(DEBUG_SERIAL_CONFIG_VALUE))
$(DEBUG_SERIAL_STAMP): FORCE
endif
ifneq ($(FRAME_TRACE_STAMP_VALUE),$(OS_ENABLE_FRAME_TRACE))
$(FRAME_TRACE_STAMP): FORCE
endif

# GNU objcopy's binary backend derives symbols from its input pathname and
# replaces every non-alphanumeric separator with an underscore.  Keep the
# spelling in one place so embedded user-program blobs work with an alternate
# BUILD directory as well as the default build/ directory.
binary_input_symbol = $(subst -,_,$(subst .,_,$(subst /,_,$(1))))
LOADER_OBJECTS = $(BUILD)/loader/main.o $(BUILD)/loader/elf.o $(BUILD)/loader/sha256.o \
                 $(BUILD)/loader/memory_map.o

KERNEL_PE = $(BUILD)/kernel/kernel.pe
KERNEL_ELF = $(BUILD)/esp/EFI/OS/kernel.elf
KERNEL_BUILD_ID = $(BUILD)/esp/EFI/OS/kernel.build-id
KERNEL_SYMBOLS = $(BUILD)/kernel/kernel.sym
NVIDIA_PORT_DIR = kernel/drivers/gpu/nvidia/port
NVIDIA_PORT_NAMES = memory atomic cache sync time thread worker vm pci mmio vbios dma irq firmware registry events
NVIDIA_PORT_SOURCES = $(addprefix $(NVIDIA_PORT_DIR)/nv_port_,$(addsuffix .c,$(NVIDIA_PORT_NAMES)))
NVIDIA_PORT_OBJECTS = $(addprefix $(BUILD)/kernel/nvidia-port-,$(addsuffix .o,$(NVIDIA_PORT_NAMES)))
NVIDIA_RM_DIR = kernel/drivers/gpu/nvidia/rm
NVIDIA_RM_DRIVER_OBJECTS = $(BUILD)/kernel/nvidia-rm-driver.o
KERNEL_LINK = $(LD) $(KERNEL_LDFLAGS)
KERNEL_OBJECT_FORMAT = pe-x86-64
include tools/nvidia/driver.mk
NVIDIA_PORT_HOST_OBJECTS = $(addprefix $(BUILD)/nvidia/host-test/nv_port_,$(addsuffix .o,$(NVIDIA_PORT_NAMES)))
USER_AUDIOD_PE = $(BUILD)/user/audiod.pe
USER_AUDIOD_ELF = $(BUILD)/user/audiod.elf
USER_AUDIOD_OBJECTS = $(BUILD)/user/audiod-service.o $(BUILD)/user/audiod-mixer.o
USER_AUDIOD_LDFLAGS = -nostdlib -Wl,--entry,audiod_entry -Wl,--subsystem,0 \
                     -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                     -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import
USER_WGET_PE = $(BUILD)/user/wget.pe
USER_WGET_ELF = $(BUILD)/user/wget.elf
USER_WGET_OBJECTS = $(BUILD)/user/wget-start.o $(BUILD)/user/wget-main.o \
                    $(BUILD)/user/wget-tls.o $(USER_LIBC_LINK_OBJECTS) \
                    $(BUILD)/user/wget-trust-anchors.o
USER_WGET_LDFLAGS = -nostdlib -Wl,--entry,wget_entry -Wl,--subsystem,0 \
                     -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                     -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import

# OpenSSL is built as freestanding static libraries for the Wget user image.
# Keep this configuration independent from USER_CFLAGS: OpenSSL needs the
# SysV ABI and SSE for its C floating-point parameter helpers, while the rest
# of the legacy user image deliberately disables SSE. The kernel saves the
# user FPU state on context switches.
OPENSSL_DIR = third_party/openssl
include $(OPENSSL_DIR)/sources.mk
OPENSSL_CRYPTO_SOURCES = $(addprefix $(OPENSSL_DIR)/,$(OPENSSL_CRYPTO_SOURCE_FILES))
OPENSSL_SSL_SOURCES = $(addprefix $(OPENSSL_DIR)/,$(OPENSSL_SSL_SOURCE_FILES))
OPENSSL_CRYPTO_OBJECTS = $(patsubst $(OPENSSL_DIR)/%.c,$(BUILD)/openssl/%.o,$(OPENSSL_CRYPTO_SOURCES))
OPENSSL_SSL_OBJECTS = $(patsubst $(OPENSSL_DIR)/%.c,$(BUILD)/openssl/%.o,$(OPENSSL_SSL_SOURCES))
OPENSSL_CRYPTO_LIB = $(BUILD)/openssl/libcrypto.a
OPENSSL_SSL_LIB = $(BUILD)/openssl/libssl.a
OPENSSL_CRYPTO_OBJECT_LIST = $(BUILD)/openssl/crypto.objects
OPENSSL_SSL_OBJECT_LIST = $(BUILD)/openssl/ssl.objects
OPENSSL_LIBS = $(OPENSSL_SSL_LIB) $(OPENSSL_CRYPTO_LIB)
OPENSSL_DEPENDENCY_FILES = $(shell find $(BUILD)/openssl -type f -name '*.d' -print 2>/dev/null)
OPENSSL_PLATFORM_CFLAGS = -DOPENSSL_SYS_FREESTANDING -U_WIN32 -U_WIN64 \
                          -U__WIN32__ -U__MINGW32__ -U__MINGW64__ \
                          -UWIN32 -UWIN64 -UWINNT -U__WIN32 -U__WIN64 \
                          -U__WINNT -U__WIN64__ -U__WINNT__
OPENSSL_CFLAGS = -O2 -ffreestanding -fno-builtin \
                 -fno-stack-protector -fno-stack-check \
                 -fno-asynchronous-unwind-tables -fno-unwind-tables \
                 -mno-red-zone -mno-mmx -mno-stack-arg-probe -msse -mabi=sysv \
                 -DOPENSSL_BUILDING_OPENSSL $(OPENSSL_PLATFORM_CFLAGS) \
                 -DOPENSSL_NO_POSIX_IO -DNO_SYSLOG -DNO_SYS_PARAM_H -DNO_SYS_UN_H \
                 -DSIXTY_FOUR_BIT -I$(OPENSSL_DIR) -I$(OPENSSL_DIR)/include \
                 -I$(OPENSSL_DIR)/providers/common/include \
                 -I$(OPENSSL_DIR)/providers/implementations/include \
                 -I$(OPENSSL_DIR)/providers/fips/include -Iinclude \
                 -Iuser/libc/include \
                 -DOPENSSLDIR=\"/EFI/OS/ssl\" \
                 -DENGINESDIR=\"/lib/engines-3\" \
                 -DMODULESDIR=\"/lib/ossl-modules\" \
                 -DNDEBUG -MMD -MP

# Blend2D is compiled for the ELF user runtime with its reference rasterizer.
# The x86_64-elf toolchain intentionally has no libstdc++ headers, therefore
# the small C++ language-library compatibility layer is kept beside the
# vendored source and the final shared object links only against System libc.
BLEND2D_DIR = third_party/blend2d
include $(BLEND2D_DIR)/sources.mk
# System currently uses the reference Blend2D pipeline: its native VM ABI does
# not provide the executable-memory contract required by AsmJit.  Keep JIT an
# explicit opt-in for host experiments instead of making every desktop launch
# enter the unsupported JIT initialization path.
BLEND2D_ENABLE_JIT ?= 0
ASMJIT_DIR = $(BLEND2D_DIR)/3rdparty/asmjit
include $(BLEND2D_DIR)/asmjit-sources.mk
ifeq ($(BLEND2D_ENABLE_JIT),1)
BLEND2D_JIT_SOURCES = $(BLEND2D_DIR)/blend2d/pipeline/jit/compoppart.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/fetchgradientpart.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/fetchpart.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/fetchpatternpart.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/fetchpixelptrpart.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/fetchsolidpart.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/fetchutilscoverage.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/fetchutilsinlineloops.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/fetchutilspixelaccess.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/fetchutilspixelgather.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/fillpart.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/pipecompiler.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/pipecomposer.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/pipefunction.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/pipegenruntime.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/pipepart.cpp \
                      $(BLEND2D_DIR)/blend2d/pipeline/jit/pipeprimitives.cpp
BLEND2D_ASMJIT_SOURCES = $(addprefix $(ASMJIT_DIR)/,$(ASMJIT_SOURCE_FILES))
else
BLEND2D_JIT_SOURCES =
BLEND2D_ASMJIT_SOURCES =
endif
BLEND2D_SOURCES = $(addprefix $(BLEND2D_DIR)/,$(BLEND2D_SOURCE_FILES)) \
                  $(BLEND2D_JIT_SOURCES)
BLEND2D_OBJECTS = $(patsubst $(BLEND2D_DIR)/%.cpp,$(BUILD)/blend2d/%.o,$(BLEND2D_SOURCES))
asmjit_object = $(BUILD)/blend2d/asmjit-$(subst /,-,$(patsubst %.cpp,%,$(subst $(ASMJIT_DIR)/,,$(1)))).o
BLEND2D_ASMJIT_OBJECTS = $(foreach source,$(BLEND2D_ASMJIT_SOURCES),$(call asmjit_object,$(source)))
BLEND2D_ALL_OBJECTS = $(BLEND2D_OBJECTS) $(BLEND2D_ASMJIT_OBJECTS)
BLEND2D_LIB = $(BUILD)/blend2d/libblend2d.a
BLEND2D_OBJECT_LIST = $(BUILD)/blend2d/objects
BLEND2D_DEPENDENCY_FILES = $(shell find $(BUILD)/blend2d -type f -name '*.d' -print 2>/dev/null)
BLEND2D_PLATFORM_OBJECT = $(ELF_RUNTIME_DIR)/blend2d-compiler.o
BLEND2D_CXX_RUNTIME_OBJECT = $(ELF_RUNTIME_DIR)/blend2d-cxx-runtime.o
BLEND2D_API_TEST_OBJECT = $(ELF_RUNTIME_DIR)/blend2d-api-test.o
BLEND2D_CXXFLAGS = -O2 -Wall -Wextra -Werror -Wno-unused-function \
                   -ffreestanding -fno-builtin -fno-stack-protector \
                   -fno-stack-check -fno-exceptions -fno-rtti \
                   -fno-threadsafe-statics -fno-use-cxa-atexit \
                   -fno-asynchronous-unwind-tables -fno-unwind-tables \
                   -fno-pic -fPIC -mno-red-zone -mno-mmx \
                   -mno-stack-arg-probe -msse2 -mabi=sysv \
                   -DBL_STATIC -DBL_BUILD_NO_TLS \
                   -DBL_BUILD_NO_FUTEX -DBL_BUILD_NO_STDCXX -DBL_BUILD_OPT_AVX512 \
                   -DBL_PLATFORM_FREESTANDING -DNDEBUG \
                   -I$(BLEND2D_DIR)/platform/cxx \
                   -I$(BLEND2D_DIR)/platform/include \
                   -I$(BLEND2D_DIR) -I$(ASMJIT_DIR) -Iinclude \
                   -Iuser/libc/include -MMD -MP
# The native Linux compiler defines __linux__, but these sources are being
# compiled against System' guest sys/stat.h, not the host libc ABI.  Prevent
# Blend2D from selecting Linux-only st_mtim/stat feature branches; the
# System ABI intentionally exposes the portable st_mtime field instead.
BLEND2D_CXXFLAGS += -U__linux__
ifeq ($(BLEND2D_ENABLE_JIT),1)
BLEND2D_CXXFLAGS += -DASMJIT_STATIC -DASMJIT_NO_FOREIGN -DASMJIT_NO_STDCXX \
                    -DASMJIT_NO_SHM_OPEN -DASMJIT_ABI_NAMESPACE=abi_bl
else
BLEND2D_CXXFLAGS += -DBL_BUILD_NO_JIT
endif

# The MinGW compiler predefines Windows compatibility macros. Clear all of
# them so OpenSSL selects the System POSIX-shaped ABI instead of the host CRT.
USER_NETD_PE = $(BUILD)/user/netd.pe
USER_NETD_ELF = $(BUILD)/user/netd.elf
USER_NETD_LDFLAGS = -nostdlib -Wl,--entry,netd_entry -Wl,--subsystem,0 \
                    -Wl,--image-base,0x400000 -Wl,--section-alignment,0x1000 \
                    -Wl,--file-alignment,0x1000 -Wl,--disable-auto-import

.PHONY: all clean esp loader kernel openssl blend2d nasm ndisasm wget debug-image debug-locations roadmap-stage-layout roadmap-stages qemu-test qemu-matrix qemu-vfio-5070 audit refactor-benchmark refactor-layout test header-sanity stage-sanity abi-sanity dynamic-loader-sanity libc-sanity libc-header-sanity bluetooth-test firmware-test audiod-test nvidia-port-test wait-subscription-test frame-trace-test chrome-start-test release-metadata FORCE

FORCE:

$(DEBUG_SERIAL_STAMP): | $(BUILD)/kernel
	@printf '%s\n' '$(DEBUG_SERIAL_CONFIG_VALUE)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(FRAME_TRACE_STAMP): | $(BUILD)/kernel
	@printf '%s\n' '$(OS_ENABLE_FRAME_TRACE)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(PHASE13_STAMP): FORCE | $(BUILD)/kernel
	@printf '%s\n' '$(OS_PHASE13)' > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; else rm -f $@.tmp; fi

all: esp

openssl: $(OPENSSL_LIBS)

blend2d: $(BLEND2D_LIB)

nasm: $(DYNAMIC_NASM_ELF)

ndisasm: $(DYNAMIC_NDISASM_ELF)

wget: $(USER_WGET_PE)

debug-image:
	$(MAKE) -f GNUmakefile esp BUILD="$(BUILD)" DEBUG=2 OS_DEBUG_SERIAL=1

debug-locations:
	BUILD="$(BUILD)" ./tools/verify-debug-locations.sh "$(KERNEL_ELF)" "$(STAGE_LOG)"

roadmap-stage-layout:
	./tools/verify-roadmap-stages.sh --static

roadmap-stages: roadmap-stage-layout
	BUILD="$(BUILD)" ./tools/verify-roadmap-stages.sh "$(KERNEL_ELF)" "$(STAGE_LOG)"

audit:
	BUILD="$(BUILD)" ./tools/audit-environment.sh

qemu-test: debug-image
	BUILD="$(BUILD)" ./run-qemu.sh --headless --cpu "$(QEMU_TEST_CPU)" \
		--seconds "$(QEMU_TEST_SECONDS)" --no-build
	BUILD="$(BUILD)" ./tools/verify-qemu-stages.sh "$(STAGE_LOG)"

qemu-matrix: debug-image
	BUILD="$(BUILD)" QEMU_MATRIX_SECONDS="$(QEMU_MATRIX_SECONDS)" \
	QEMU_MATRIX_CPUS="$(QEMU_MATRIX_CPUS)" ./tools/run-qemu-matrix.sh

qemu-vfio-5070:
	BUILD="$(BUILD)" ./tools/run-qemu-vfio-5070.sh --build "$(BUILD)"

refactor-benchmark:
	BUILD="$(BUILD)" BENCHMARK_RUNS="$(BENCHMARK_RUNS)" \
	BENCHMARK_CPU="$(BENCHMARK_CPU)" BENCHMARK_SECONDS="$(BENCHMARK_SECONDS)" \
	./tools/refactor-benchmark.sh

refactor-layout:
	@bash ./tools/verify-refactor-layout.sh

$(BUILD)/loader $(BUILD)/kernel $(BUILD)/user $(BUILD)/openssl $(BUILD)/blend2d $(BUILD)/nasm $(ELF_RUNTIME_DIR) \
$(BUILD)/esp $(BUILD)/esp/EFI/BOOT $(BUILD)/esp/EFI/OS \
$(BUILD)/esp/EFI/OS/fonts:
	mkdir -p $@

$(BUILD)/esp/boot $(BUILD)/esp/lib $(BUILD)/esp/sbin $(BUILD)/esp/tmp $(BUILD)/esp/log:
	mkdir -p $@

# 桌面专属壁纸与界面放在桌面应用目录，公共字体仍保留在系统字体目录。
DESKTOP_ASSET_DIR = $(BUILD)/esp/usr/apps/desktop
FONT_ASSET = $(BUILD)/esp/EFI/OS/fonts/default.ttf
VFS_SELF_TEST_SEED = $(BUILD)/esp/EFI/OS/vfsmap.tst
DESKTOP_ASSETS = $(DESKTOP_ASSET_DIR)/wall.png \
	$(BUILD)/esp/usr/apps/desktop/desktop.ui \
	$(BUILD)/esp/usr/apps/fileman/icon.png \
	$(foreach app,gshell notepad settings taskmgr chrome imageview,$(BUILD)/esp/usr/apps/$(app)/icon.png) \
	$(FONT_ASSET) $(BUILD)/esp/EFI/OS/fonts/cjk.ttf $(BUILD)/esp/EFI/OS/fonts/title-cjk-LICENSE

$(FONT_ASSET): assets/fonts/default.ttf | $(BUILD)/esp/EFI/OS/fonts
	cp $< $@

$(BUILD)/esp/EFI/OS/fonts/cjk.ttf: assets/fonts/title-cjk.ttf | $(BUILD)/esp/EFI/OS/fonts
	cp $< $@

$(BUILD)/esp/EFI/OS/fonts/title-cjk-LICENSE: assets/fonts/title-cjk-LICENSE | $(BUILD)/esp/EFI/OS/fonts
	cp $< $@

$(DESKTOP_ASSET_DIR):
	mkdir -p $@

$(DESKTOP_ASSET_DIR)/wall.png: assets/desktop/wallpaper.png | $(DESKTOP_ASSET_DIR)
	cp $< $@

$(BUILD)/esp/usr/apps/desktop/desktop.ui: user/ui/desktop.ui
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/fileman/icon.png: assets/desktop/file-manager-icon.png
	mkdir -p $(@D)
	cp $< $@

# 图标和可执行文件属于同一个应用包，不再共享带位置编号的图集。
define APP_ICON_RULE
$(BUILD)/esp/usr/apps/$(1)/icon.png: assets/desktop/whitesur/$(2).svg tools/render-app-icon.py
	python3 tools/render-app-icon.py $$< $$@
endef
$(eval $(call APP_ICON_RULE,gshell,terminal))
$(eval $(call APP_ICON_RULE,notepad,accessories-text-editor))
$(eval $(call APP_ICON_RULE,settings,preferences-system))
$(eval $(call APP_ICON_RULE,imageview,image-viewer))
$(eval $(call APP_ICON_RULE,taskmgr,utilities-system-monitor))
$(eval $(call APP_ICON_RULE,chrome,google-chrome))

$(VFS_SELF_TEST_SEED): assets/desktop/vfsmap.tst | $(BUILD)/esp/EFI/OS
	cp $< $@
	truncate -s 5 $@

$(BUILD)/loader/%.o: boot/uefi/%.c $(DEBUG_SERIAL_STAMP) | $(BUILD)/loader
	$(CC) $(COMMON_CFLAGS) -c $< -o $@

$(BUILD)/loader/%.o: boot/uefi/%.S $(DEBUG_SERIAL_STAMP) | $(BUILD)/loader
	$(CC) $(COMMON_CFLAGS) -c $< -o $@

$(BUILD)/user/audiod-service.o: user/services/audiod/service.c | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/audiod-mixer.o: user/services/audiod/mixer.c | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/ld.o: user/runtime/ld.c user/runtime/elf64.h | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_RUNTIME_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/ld-start.o: user/runtime/ld_start.S | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_RUNTIME_CFLAGS) -c $< -o $@

$(DYNAMIC_LOADER_ELF): makefile $(ELF_RUNTIME_DIR)/ld.o $(ELF_RUNTIME_DIR)/ld-start.o | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -shared -e ld_entry \
		--soname ld-system.so.1 --hash-style=sysv -Bsymbolic \
		-z now -z text $(filter %.o,$^) -o $@

$(ELF_RUNTIME_DIR)/libc-%.o: user/libc/%.c \
		user/libc/include/libc/libc.h | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_RUNTIME_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/libc-memory.o: include/uapi/memory.h include/uapi/syscall.h
$(ELF_RUNTIME_DIR)/libc-mman.o $(ELF_RUNTIME_DIR)/libc-posix_compat.o: \
		include/uapi/memory.h

$(ELF_RUNTIME_DIR)/libc-mman.o: user/libc/include/unistd.h
$(ELF_RUNTIME_DIR)/libc-posix_compat.o: user/libc/include/unistd.h

$(ELF_RUNTIME_DIR)/libc-asm-%.o: user/libc/%.S | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_RUNTIME_CFLAGS) -c $< -o $@

$(DYNAMIC_LIBC_ELF): makefile $(DYNAMIC_LIBC_OBJECTS) | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -shared -e 0 \
		--soname libsystemc.so.1 --hash-style=sysv -Bsymbolic \
		-z now -z text $(filter %.o,$^) -o $@

$(ELF_RUNTIME_DIR)/libc-crt-start.o: user/libc/crt_start.S | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/libc-test.o: user/runtime/libc_test.c \
		user/libc/include/libc/libc.h user/runtime/blend2d_api_test.h \
		user/runtime/text.h | \
		$(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(BLEND2D_API_TEST_OBJECT): user/runtime/blend2d_api_test.c \
		user/runtime/blend2d_api_test.h | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(DYNAMIC_LIBC_TEST_ELF): makefile $(ELF_RUNTIME_DIR)/libc-crt-start.o \
		$(ELF_RUNTIME_DIR)/libc-startup.o $(ELF_RUNTIME_DIR)/libc-test.o \
		$(BLEND2D_API_TEST_OBJECT) $(ELF_RUNTIME_DIR)/text.o \
		$(DYNAMIC_LIBC_ELF) $(DYNAMIC_GFX_LIBRARY_ELF) | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -pie -e libc_crt_entry \
		-dynamic-linker /lib/ld-system.so.1 --hash-style=sysv \
		--no-as-needed $(filter %.o %.so.1,$^) -o $@

$(ELF_RUNTIME_DIR)/linux-events-test.o: tools/chromium/linux_events_test.c \
		include/uapi/memory.h user/libc/include/unistd.h \
		user/libc/include/sys/sysinfo.h | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(DYNAMIC_LINUX_EVENTS_TEST_ELF): makefile $(ELF_RUNTIME_DIR)/libc-crt-start.o \
		$(ELF_RUNTIME_DIR)/libc-startup.o \
		$(ELF_RUNTIME_DIR)/linux-events-test.o $(DYNAMIC_LIBC_ELF) | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -pie -e libc_crt_entry \
		-dynamic-linker /lib/ld-linux-system.so.1 --hash-style=sysv \
		--no-as-needed $(filter %.o %.so.1,$^) -o $@

$(ELF_RUNTIME_DIR)/text.o: user/runtime/text.c \
		user/runtime/text.h user/runtime/text_metrics.h | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/text-layout.o: user/runtime/text_layout.c \
		user/runtime/text.h user/runtime/text_metrics.h \
		include/uapi/control_tree.h include/uapi/utf8.h | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/imageview.o: user/desktop/imageview/main.c \
		user/runtime/file_picker.h user/runtime/message_box.h user/runtime/modal_tree.h \
		user/runtime/text.h user/runtime/caption.h include/uapi/control_tree.h \
		include/uapi/window_theme.h include/uapi/image.h | \
		$(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(BUILD)/nasm/%.o: $(NASM_DIR)/%.c tools/nasm-platform/config/config.h \
		$(NASM_DIR)/config/unconfig.h $(NASM_DIR)/autoconf/attribute.h | \
		$(BUILD)/nasm
	mkdir -p $(@D)
	$(ELFCC) $(NASM_CFLAGS) -c $< -o $@

$(DYNAMIC_NASM_ELF): makefile $(ELF_RUNTIME_DIR)/libc-crt-start.o \
		$(ELF_RUNTIME_DIR)/libc-startup.o $(NASM_OBJECTS) \
		$(DYNAMIC_LIBC_ELF) | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -pie -e libc_crt_entry \
		-dynamic-linker /lib/ld-system.so.1 --hash-style=sysv \
		--no-as-needed $(filter %.o %.so.1,$^) -o $@

$(DYNAMIC_NDISASM_ELF): makefile $(ELF_RUNTIME_DIR)/libc-crt-start.o \
		$(ELF_RUNTIME_DIR)/libc-startup.o $(NDISASM_OBJECTS) \
		$(DYNAMIC_LIBC_ELF) | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -pie -e libc_crt_entry \
		-dynamic-linker /lib/ld-system.so.1 --hash-style=sysv \
		--no-as-needed $(filter %.o %.so.1,$^) -o $@

$(DYNAMIC_IMAGEVIEW_ELF): makefile $(ELF_RUNTIME_DIR)/libc-crt-start.o \
		$(ELF_RUNTIME_DIR)/libc-startup.o $(ELF_RUNTIME_DIR)/imageview.o \
		$(ELF_RUNTIME_DIR)/text-layout.o $(DYNAMIC_LIBC_ELF) | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -pie -e libc_crt_entry \
		-dynamic-linker /lib/ld-system.so.1 --hash-style=sysv \
		--no-as-needed $(filter %.o %.so.1,$^) -o $@

define DYNAMIC_TEXT_APP_RULE
$(ELF_RUNTIME_DIR)/$(1).o: $(2) user/runtime/text.h user/runtime/caption.h user/runtime/paint.h user/client_chrome.h include/uapi/window_theme.h include/uapi/window.h include/uapi/control_tree.h include/uapi/utf8.h | $(ELF_RUNTIME_DIR)
	$$(ELFCC) $$(ELF_APP_CFLAGS) -c $$< -o $$@


$(ELF_RUNTIME_DIR)/$(1).elf: makefile $(ELF_RUNTIME_DIR)/libc-crt-start.o \
		$$(ELF_RUNTIME_DIR)/libc-startup.o $(ELF_RUNTIME_DIR)/$(1).o \
		$$(ELF_RUNTIME_DIR)/text-layout.o $$(DYNAMIC_LIBC_ELF) | $$(ELF_RUNTIME_DIR)
	$$(ELFLD) $$(ELF_LINK_FLAGS) -pie -e libc_crt_entry \
		-dynamic-linker $(if $(filter chrome-start,$(1)),/lib/ld-linux-system.so.1,/lib/ld-system.so.1) --hash-style=sysv \
		--no-as-needed $$(filter %.o %.so.1,$$^) -o $$@
endef

$(eval $(call DYNAMIC_TEXT_APP_RULE,gshell,user/desktop/gshell/main.c))
# The launcher has no window/text renderer. Do not load the graphics library
# merely to select arguments and exec the browser.
$(ELF_RUNTIME_DIR)/chrome-start.o: user/runtime/chrome_start.c | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(DYNAMIC_CHROME_START_ELF): makefile $(ELF_RUNTIME_DIR)/libc-crt-start.o \
		$(ELF_RUNTIME_DIR)/libc-startup.o $(ELF_RUNTIME_DIR)/chrome-start.o \
		$(DYNAMIC_LIBC_ELF) | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -pie -e libc_crt_entry \
		-dynamic-linker /lib/ld-linux-system.so.1 --hash-style=sysv \
		--no-as-needed $(filter %.o %.so.1,$^) -o $@
$(eval $(call DYNAMIC_TEXT_APP_RULE,notepad,user/desktop/notepad/main.c))
$(ELF_RUNTIME_DIR)/notepad.o: user/runtime/message_box.h
$(ELF_RUNTIME_DIR)/notepad.o: include/uapi/control_tree.h
$(eval $(call DYNAMIC_TEXT_APP_RULE,fileman,user/desktop/fileman/main.c))
$(ELF_RUNTIME_DIR)/fileman.o: user/desktop/fileman/content_tree.h
$(ELF_RUNTIME_DIR)/fileman.o: user/desktop/fileman/file_copy.h
$(eval $(call DYNAMIC_TEXT_APP_RULE,taskmgr,user/desktop/taskmgr/main.c))
$(ELF_RUNTIME_DIR)/taskmgr.o: include/uapi/control_tree.h
$(eval $(call DYNAMIC_TEXT_APP_RULE,settings,user/desktop/settings/main.c))
$(ELF_RUNTIME_DIR)/settings.o: user/desktop/settings/network_tree.h user/desktop/settings/system_info.h user/desktop/settings/sound.h user/desktop/settings/appearance.h user/desktop/settings/adapters.h

$(ELF_RUNTIME_DIR)/control-file.o: user/runtime/control_file.c user/runtime/control_file.h \
        include/uapi/control_tree.h include/uapi/control_validation.h include/uapi/window_theme.h | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/ui-file-demo.o: user/runtime/ui_file_demo.c user/runtime/control_file.h \
        include/uapi/control_tree.h include/uapi/window.h | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/ui-file-demo.elf: $(ELF_RUNTIME_DIR)/ui-file-demo.o $(ELF_RUNTIME_DIR)/control-file.o \
        $(ELF_RUNTIME_DIR)/libc-crt-start.o $(ELF_RUNTIME_DIR)/libc-startup.o $(DYNAMIC_LIBC_ELF)
	$(ELFLD) $(ELF_LINK_FLAGS) -pie -e libc_crt_entry -dynamic-linker /lib/ld-system.so.1 \
		--hash-style=sysv --no-as-needed $(filter %.o %.so.1,$^) -o $@

$(BUILD)/esp/usr/apps/ui-file-demo/ui-file-demo: $(ELF_RUNTIME_DIR)/ui-file-demo.elf | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/ui-file-demo/controls.ui: user/ui/controls.ui
	mkdir -p $(@D)
	cp $< $@

$(ELF_RUNTIME_DIR)/graphics.o: user/runtime/graphics.c \
		user/runtime/graphics.h $(BLEND2D_DIR)/blend2d/blend2d.h | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_RUNTIME_CFLAGS) -I$(BLEND2D_DIR) -c $< -o $@

$(BLEND2D_PLATFORM_OBJECT): $(BLEND2D_DIR)/platform/compiler.c | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_RUNTIME_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/fat-fsync-probe.o: tests/user/fat_fsync_probe.c | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(DYNAMIC_FSYNC_PROBE_ELF): makefile $(ELF_RUNTIME_DIR)/libc-crt-start.o \
        $(ELF_RUNTIME_DIR)/libc-startup.o $(ELF_RUNTIME_DIR)/fat-fsync-probe.o \
        $(DYNAMIC_LIBC_ELF) | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -pie -e libc_crt_entry \
		-dynamic-linker /lib/ld-system.so.1 --hash-style=sysv \
		--no-as-needed $(filter %.o %.so.1,$^) -o $@

.PHONY: fat-fsync-probe
fat-fsync-probe: $(BUILD)/esp/usr/apps/fat-fsync-probe/fat-fsync-probe

$(BLEND2D_CXX_RUNTIME_OBJECT): $(BLEND2D_DIR)/platform/cxx_runtime.cpp | $(ELF_RUNTIME_DIR)
	$(ELFCXX) $(BLEND2D_CXXFLAGS) -c $< -o $@

$(BUILD)/blend2d/%.o: $(BLEND2D_DIR)/%.cpp | $(BUILD)/blend2d
	mkdir -p $(@D)
	$(ELFCXX) $(BLEND2D_CXXFLAGS) $(call blend2d_source_flags,$<) -c $< -o $@

blend2d_source_flags = $(if $(findstring _avx2.cpp,$(1)),-mpopcnt -mpclmul -mbmi -mbmi2 -mavx2 -DBL_TARGET_OPT_POPCNT -DBL_TARGET_OPT_BMI2,$(if $(findstring _avx.cpp,$(1)),-mpopcnt -mpclmul -mavx,$(if $(findstring _sse4_2.cpp,$(1)),-mpopcnt -mpclmul -msse4.2,$(if $(findstring _ssse3.cpp,$(1)),-mssse3,$(if $(findstring _sse2.cpp,$(1)),-msse2,)))))

define BLEND2D_ASMJIT_OBJECT_RULE
$(call asmjit_object,$(1)): $(1) | $(BUILD)/blend2d
	mkdir -p $$(@D)
	$(ELFCXX) $(BLEND2D_CXXFLAGS) -c $$< -o $$@
endef
$(foreach source,$(BLEND2D_ASMJIT_SOURCES),$(eval $(call BLEND2D_ASMJIT_OBJECT_RULE,$(source))))

$(BLEND2D_OBJECT_LIST): $(BLEND2D_DIR)/sources.mk $(BLEND2D_DIR)/asmjit-sources.mk \
		$(BLEND2D_ALL_OBJECTS) | $(BUILD)/blend2d
	$(file >$@)
	$(foreach object,$(BLEND2D_ALL_OBJECTS),$(file >>$@,$(object)))
	@:

$(BLEND2D_LIB): $(BLEND2D_OBJECT_LIST) tools/archive-objects.sh | $(BUILD)/blend2d
	sh tools/archive-objects.sh "$(ELFTOOLPREFIX)ar" "$@" "$<"

$(DYNAMIC_GFX_LIBRARY_ELF): $(ELF_RUNTIME_DIR)/graphics.o \
                             $(BLEND2D_PLATFORM_OBJECT) $(BLEND2D_CXX_RUNTIME_OBJECT) $(BLEND2D_LIB) \
                             $(DYNAMIC_LIBC_OBJECTS) | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -shared -e 0 --soname libgraphics.so.1 \
		--hash-style=sysv -Bsymbolic -z now -z text \
		--start-group $(ELF_RUNTIME_DIR)/graphics.o \
			$(BLEND2D_PLATFORM_OBJECT) $(BLEND2D_CXX_RUNTIME_OBJECT) \
			$(DYNAMIC_LIBC_OBJECTS) --end-group \
		--whole-archive $(BLEND2D_LIB) --no-whole-archive -o $@

$(ELF_RUNTIME_DIR)/dyn-gfx.o: user/runtime/dyn_gfx.c \
		user/runtime/graphics.h user/runtime/blend2d_demo.h | \
		$(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/blend2d-demo.o: user/runtime/blend2d_demo.c \
		user/runtime/blend2d_demo.h | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/dyn-gfx-start.o: user/runtime/dyn_gfx_start.S | $(ELF_RUNTIME_DIR)
	$(ELFCC) $(ELF_APP_CFLAGS) -c $< -o $@

$(DYNAMIC_GFX_TEST_ELF): makefile $(ELF_RUNTIME_DIR)/dyn-gfx-start.o \
		$(ELF_RUNTIME_DIR)/dyn-gfx.o $(ELF_RUNTIME_DIR)/blend2d-demo.o \
		$(DYNAMIC_GFX_LIBRARY_ELF) | $(ELF_RUNTIME_DIR)
	$(ELFLD) $(ELF_LINK_FLAGS) -pie -e dyn_gfx_entry \
		-dynamic-linker /lib/ld-system.so.1 --hash-style=sysv \
		--no-as-needed $(filter %.o %.so.1,$^) -o $@

$(BUILD)/user/libc-%.o: user/libc/%.c user/libc/include/libc/libc.h | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/libc-memory.o: include/uapi/memory.h include/uapi/syscall.h
$(BUILD)/user/libc-mman.o $(BUILD)/user/libc-posix_compat.o: \
		include/uapi/memory.h

$(BUILD)/user/libc-mman.o: user/libc/include/unistd.h
$(BUILD)/user/libc-posix_compat.o: user/libc/include/unistd.h

$(BUILD)/user/libc-asm-%.o: user/libc/%.S | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_AUDIOD_PE): $(USER_AUDIOD_OBJECTS) | $(BUILD)/user
	$(LD) $(USER_AUDIOD_LDFLAGS) $^ -o $@

$(USER_AUDIOD_ELF): $(USER_AUDIOD_PE) | $(BUILD)/user
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/user/netd.o: user/services/networkd/netd.c | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_NETD_PE): $(BUILD)/user/netd.o $(USER_LIBC_LINK_OBJECTS) | $(BUILD)/user
	$(LD) $(USER_NETD_LDFLAGS) $^ -o $@

$(USER_NETD_ELF): $(USER_NETD_PE) | $(BUILD)/user
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/user/wget-start.o: user/desktop/wget/start.S | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/wget-main.o: user/desktop/wget/main.c | $(BUILD)/user
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/wget-tls.o: user/desktop/wget/tls.c user/desktop/wget/tls.h | $(BUILD)/user
	$(CC) $(USER_CFLAGS) $(OPENSSL_PLATFORM_CFLAGS) -I$(OPENSSL_DIR) -I$(OPENSSL_DIR)/include \
		-I$(OPENSSL_DIR)/providers/common/include \
		-I$(OPENSSL_DIR)/providers/implementations/include -c $< -o $@

$(BUILD)/user/wget-trust-anchors.o: user/desktop/wget/trust_anchors.c | $(BUILD)/user
	$(CC) $(USER_CFLAGS) $(OPENSSL_PLATFORM_CFLAGS) -I$(OPENSSL_DIR) -I$(OPENSSL_DIR)/include \
		-I$(OPENSSL_DIR)/providers/common/include \
		-I$(OPENSSL_DIR)/providers/implementations/include -c $< -o $@

$(BUILD)/openssl/%.o: $(OPENSSL_DIR)/%.c
	mkdir -p $(@D)
	$(CC) $(OPENSSL_CFLAGS) -c $< -o $@

$(OPENSSL_CRYPTO_OBJECT_LIST): $(OPENSSL_DIR)/sources.mk $(OPENSSL_CRYPTO_OBJECTS) | $(BUILD)/openssl
	$(file >$@)
	$(foreach object,$(OPENSSL_CRYPTO_OBJECTS),$(file >>$@,$(object)))
	@:

$(OPENSSL_SSL_OBJECT_LIST): $(OPENSSL_DIR)/sources.mk $(OPENSSL_SSL_OBJECTS) | $(BUILD)/openssl
	$(file >$@)
	$(foreach object,$(OPENSSL_SSL_OBJECTS),$(file >>$@,$(object)))
	@:

$(OPENSSL_CRYPTO_LIB): $(OPENSSL_CRYPTO_OBJECT_LIST) tools/archive-objects.sh | $(BUILD)/openssl
	sh tools/archive-objects.sh "$(AR)" "$@" "$<"

$(OPENSSL_SSL_LIB): $(OPENSSL_SSL_OBJECT_LIST) tools/archive-objects.sh | $(BUILD)/openssl
	sh tools/archive-objects.sh "$(AR)" "$@" "$<"

$(USER_WGET_PE): $(USER_WGET_OBJECTS) $(OPENSSL_LIBS) | $(BUILD)/user
	$(LD) $(USER_WGET_LDFLAGS) -Wl,--start-group $^ -Wl,--end-group -o $@

$(USER_WGET_ELF): $(USER_WGET_PE) | $(BUILD)/user
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/kernel/entry.o: kernel/init/main.c \
		include/kernel/console_backend.h \
		include/kernel/init_filesystem.h include/kernel/init_runtime.h \
		include/kernel/init_self_tests.h \
		include/kernel/realtest.h \
		$(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/early-init.o: kernel/init/early.c $(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/memory-init.o: kernel/init/memory.c include/kernel/mm.h \
		$(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/devices-init.o: kernel/init/devices.c include/kernel/realtest.h \
		$(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvidia-rm-driver.o: $(NVIDIA_RM_DIR)/nv_rm_driver.c \
		$(NVIDIA_RM_DIR)/include/nv_rm_driver.h \
		$(NVIDIA_RM_DIR)/include/nv_rm_vendor_bridge.h \
		$(NVIDIA_PORT_DIR)/include/nv_port.h $(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -I$(NVIDIA_PORT_DIR)/include \
		-I$(NVIDIA_RM_DIR)/include -c $< -o $@

$(BUILD)/kernel/core-init.o: kernel/init/core.c $(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/storage-init.o: kernel/init/storage.c $(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/scheduler-init.o: kernel/init/scheduler.c $(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/network-init.o: kernel/init/network.c \
		include/kernel/rtl8126.h $(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/filesystem-init.o: kernel/init/filesystem.c \
        include/kernel/init_filesystem.h include/kernel/bootinfo.h \
        include/kernel/realtest.h \
		$(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/filesystem-self-test.o: kernel/init/filesystem_self_test.c \
		kernel/init/filesystem_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/filesystem-root.o: kernel/init/filesystem_root.c \
        kernel/init/filesystem_root_internal.h include/kernel/bootinfo.h \
        include/kernel/realtest.h | \
		$(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-init.o: kernel/init/graphics.c $(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/userspace-init.o: kernel/init/userspace.c $(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/init-self-tests.o: kernel/init/self_tests.c \
		include/kernel/init_self_tests.h include/kernel/init_runtime.h \
		$(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/runtime-init.o: kernel/init/runtime.c \
		include/kernel/init_runtime.h include/kernel/realtest.h \
		include/kernel/linux_abi.h \
		$(DEBUG_SERIAL_STAMP) $(PHASE13_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -DOS_PHASE13=$(OS_PHASE13) -c $< -o $@

$(BUILD)/kernel/console.o: kernel/console_printf.c include/kernel/console.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/console-backend.o: kernel/console.c \
		include/kernel/console_backend.h include/kernel/realtest.h \
		include/kernel/console.h include/console_font_a8.h \
		$(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/string.o: kernel/string.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/ascii_font.o: kernel/graphics/ascii_font.c \
		kernel/graphics/ascii_font_internal.h include/ascii_font.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/ascii-font-parser.o: kernel/graphics/ascii_font_parser.c \
		kernel/graphics/ascii_font_internal.h include/ascii_font.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/paging.o: kernel/arch/x86_64/boot/early_paging.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/direct.o: kernel/mm/physmap.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/page-db.o: kernel/mm/phys/page_db.c kernel/mm/phys/internal.h \
		include/kernel/mm.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/page-alloc.o: kernel/mm/phys/page_alloc.c kernel/mm/phys/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/percpu-page.o: kernel/mm/phys/percpu_page.c kernel/mm/phys/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/page-table.o: kernel/arch/x86_64/mm/page_table.c \
		kernel/arch/x86_64/mm/mmu_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/mmu.o: kernel/arch/x86_64/mm/mmu.c \
		kernel/arch/x86_64/mm/mmu_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/kmalloc.o: kernel/mm/kmalloc.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vmalloc.o: kernel/mm/vmalloc.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vm-space.o: kernel/mm/vm_space.c kernel/mm/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vm-object.o: kernel/mm/object.c kernel/mm/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vma.o: kernel/mm/vma.c kernel/mm/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vm-map.o: kernel/mm/map.c kernel/mm/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vm-protection.o: kernel/mm/protection.c kernel/mm/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vm-shared.o: kernel/mm/shared.c kernel/mm/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vm-fault.o: kernel/mm/fault.c kernel/mm/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vm-tlb.o: kernel/mm/tlb.c kernel/mm/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/shared-section.o: kernel/ipc/shared_section.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/cpu.o: kernel/arch/x86_64/cpu/cpu.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/interrupt.o: kernel/arch/x86_64/irq/exception.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/interrupt-core.o: kernel/arch/x86_64/irq/exception.c \
		include/kernel/realtest.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/irq-core.o: kernel/arch/x86_64/irq/irq.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/uaccess-entry.o: kernel/arch/x86_64/mm/uaccess.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/uaccess.o: kernel/arch/x86_64/mm/uaccess.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall.o: kernel/arch/x86_64/syscall/entry.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/context.o: kernel/arch/x86_64/cpu/context_switch.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-entry.o: kernel/arch/x86_64/syscall/return.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/apic.o: kernel/arch/x86_64/irq/apic.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/acpi.o: kernel/drivers/acpi/tables.c \
		include/arch/x86_64/acpi.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/realtest.o: kernel/realtest.c include/kernel/realtest.h \
		include/arch/x86_64/reboot.h include/kernel/vfs.h include/kernel/fat32.h | \
		$(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/reboot.o: kernel/arch/x86_64/reboot.c \
		include/arch/x86_64/reboot.h include/arch/x86_64/acpi.h | \
		$(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/smp.o: kernel/sched/smp.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/trampoline.o: kernel/arch/x86_64/boot/trampoline.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/tlb.o: kernel/arch/x86_64/mm/tlb.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/timer.o: kernel/arch/x86_64/time/deadline.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/stack-check.o: kernel/arch/x86_64/cpu/stack_check.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/debug-stage.o: kernel/debug_stage.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-core.o: kernel/syscall/handlers.c \
		kernel/syscall/internal.h include/uapi/syscall.h \
		include/uapi/memory.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/linux-abi.o: kernel/syscall/linux.c \
		include/kernel/linux_abi.h include/uapi/linux_abi.h \
		include/uapi/display.h include/kernel/frame_trace.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/linux-phase7-test.o: kernel/process/linux_phase7_test.c \
		include/kernel/linux_abi.h include/kernel/elf_loader.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/linux-phase8-test.o: kernel/process/linux_phase8_test.c \
		include/kernel/linux_abi.h include/kernel/elf_loader.h \
		include/kernel/nv_frontend.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/linux-phase9-test.o: kernel/process/linux_phase9_test.c \
		include/kernel/linux_abi.h include/kernel/elf_loader.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/linux-phase10-test.o: kernel/process/linux_phase10_test.c \
		include/kernel/linux_abi.h include/kernel/elf_loader.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/linux-phase11-test.o: kernel/process/linux_phase11_test.c \
		include/kernel/linux_abi.h include/kernel/elf_loader.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/linux-phase12-test.o: kernel/process/linux_phase12_test.c \
		include/kernel/linux_abi.h include/kernel/elf_loader.h \
		include/kernel/window_server.h include/uapi/linux_abi.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/linux-phase13-test.o: kernel/process/linux_phase13_test.c \
		include/kernel/linux_abi.h include/kernel/elf_loader.h \
		$(PHASE13_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -DOS_PHASE13=$(OS_PHASE13) -c $< -o $@

$(BUILD)/kernel/nvidia-rm-frontend.o: \
		$(NVIDIA_RM_DIR)/nv_rm_frontend.c \
		$(NVIDIA_RM_DIR)/include/nv_rm_vendor_bridge.h \
		include/kernel/nv_frontend.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -I$(NVIDIA_RM_DIR)/include -c $< -o $@

$(BUILD)/kernel/syscall-debug.o: kernel/syscall/debug.c \
		kernel/syscall/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-network.o: kernel/syscall/network.c \
		kernel/syscall/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-graphics.o: kernel/syscall/graphics.c \
		kernel/syscall/internal.h include/kernel/frame_trace.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-font.o: kernel/syscall/font.c \
		kernel/syscall/internal.h include/ascii_font.h include/uapi/font.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-image.o: kernel/syscall/image.c \
		kernel/syscall/internal.h include/uapi/image.h \
		kernel/graphics/png.h kernel/graphics/png_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-filesystem.o: kernel/syscall/filesystem.c \
		kernel/syscall/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-pipe.o: kernel/syscall/pipe.c \
		kernel/syscall/internal.h include/kernel/pipe.h include/uapi/pipe.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-audio.o: kernel/syscall/audio.c \
		kernel/syscall/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-device.o: kernel/syscall/device.c \
		kernel/syscall/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-sync.o: kernel/syscall/sync.c \
		kernel/syscall/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-random.o: kernel/syscall/random.c \
		kernel/syscall/internal.h include/kernel/random.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-io.o: kernel/syscall/io.c \
		kernel/syscall/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-process.o: kernel/syscall/process.c \
		kernel/syscall/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-vm.o: kernel/syscall/vm.c \
		kernel/syscall/internal.h include/kernel/mm.h \
		include/uapi/memory.h include/uapi/syscall.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/syscall-handles.o: kernel/syscall/handles.c \
		kernel/syscall/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/sched-core.o: kernel/sched/core.c \
		kernel/sched/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/sched-clock.o: kernel/sched/clock.c \
		kernel/sched/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/sched-monitor.o: kernel/sched/monitor.c kernel/sched/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/sched-runqueue.o: kernel/sched/runqueue.c \
		kernel/sched/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/sched-balance.o: kernel/sched/balance.c \
		kernel/sched/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/process-core.o: kernel/process/process.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/process-thread.o: kernel/process/thread.c \
		kernel/process/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/process-exit.o: kernel/process/exit.c \
		kernel/process/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/resource.o: kernel/resource.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-elf.o: kernel/process/exec.c \
		kernel/process/exec_internal.h include/kernel/elf_loader.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-runtime-test.o: kernel/process/runtime_test.c \
		kernel/process/exec_internal.h include/kernel/elf_loader.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-services-init.o: kernel/init/user_services.c \
		include/kernel/init_user_services.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-test-blob.o: tests/user/user_test_blob.S | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-init-blob.o: kernel/process/init_blob.S \
		$(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/user-init-blob.bin: $(BUILD)/kernel/user-init-blob.o | $(BUILD)/kernel
	$(OBJCOPY) -O binary --only-section=.rdata $< $@

$(BUILD)/make-init-image.exe: tools/make_init_image.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror $< -o $@

$(BUILD)/make-test-elf.exe: tools/make_test_elf.c | $(BUILD)
	$(HOSTCC) -O2 -Wall -Wextra -Werror $< -o $@

$(BUILD)/kernel/object-core.o: kernel/object/object.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/handle.o: kernel/object/handle.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/wait.o: kernel/sync/wait.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/futex.o: kernel/sync/futex.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/mutex.o: kernel/sync/mutex.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/io-mutex.o: kernel/sync/io_mutex.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/canonical-io.o: kernel/io/request.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/completion-port.o: kernel/ipc/completion.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/message-port.o: kernel/ipc/port.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/pipe.o: kernel/ipc/pipe.c include/kernel/pipe.h \
		include/uapi/pipe.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/timer-core.o: kernel/time/hrtimer.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/deferred.o: kernel/irq/deferred.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/background.o: kernel/irq/background.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/service.o: kernel/service.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/watchdog.o: kernel/watchdog.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/power.o: kernel/power.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/crash-dump.o: kernel/panic.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/input-core.o: kernel/drivers/input/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/gpu-core.o: kernel/drivers/gpu/core/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/display-core.o: kernel/drivers/display/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/dma-core.o: kernel/drivers/core/dma.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/audio-core.o: kernel/drivers/audio/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/hda.o: kernel/drivers/audio/hda/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/bluetooth-core.o: kernel/drivers/bluetooth/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/firmware-core.o: kernel/drivers/core/firmware.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/package.o: kernel/package.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/rcu.o: kernel/sync/rcu.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/telemetry.o: kernel/trace.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/perf.o: kernel/perf.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/iommu-core.o: kernel/drivers/iommu/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/device-core.o: kernel/drivers/core/device.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/pci-core.o: kernel/drivers/pci/enumerate.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvidia-port-%.o: $(NVIDIA_PORT_DIR)/nv_port_%.c \
		$(NVIDIA_PORT_DIR)/include/nv_port.h $(DEBUG_SERIAL_STAMP) | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -I$(NVIDIA_PORT_DIR)/include -c $< -o $@

$(BUILD)/nvidia/host-test:
	mkdir -p $@

$(BUILD)/nvidia/host-test/nv_port_%.o: $(NVIDIA_PORT_DIR)/nv_port_%.c \
		$(NVIDIA_PORT_DIR)/include/nv_port.h | $(BUILD)/nvidia/host-test
	$(HOSTCC) -DNV_PORT_HOST_TEST -std=c11 -O2 -Wall -Wextra -Werror -pthread \
		-ffunction-sections -fdata-sections -Iinclude -I$(NVIDIA_PORT_DIR)/include \
		-MMD -MP -MF $@.d -c $< -o $@

$(BUILD)/nvidia/host-test/nv_port_test.o: tests/kernel/nv_port_test.c \
		$(NVIDIA_PORT_DIR)/include/nv_port.h | $(BUILD)/nvidia/host-test
	$(HOSTCC) -DNV_PORT_HOST_TEST -std=c11 -O2 -Wall -Wextra -Werror -pthread \
		-ffunction-sections -fdata-sections -Iinclude -I$(NVIDIA_PORT_DIR)/include \
		-MMD -MP -MF $@.d -c $< -o $@

$(BUILD)/nvidia-port-test.exe: $(NVIDIA_PORT_HOST_OBJECTS) \
		$(BUILD)/nvidia/host-test/nv_port_test.o kernel/drivers/core/firmware.c | $(BUILD)
	$(HOSTCC) -DNV_PORT_HOST_TEST -std=c11 -O2 -Wall -Wextra -Werror -pthread \
		-Iinclude -Wl,--gc-sections -MMD -MP -MF $@.d $(filter %.o %.c,$^) -o $@

nvidia-port-test: $(BUILD)/nvidia-port-test.exe
	./$(BUILD)/nvidia-port-test.exe

$(BUILD)/kernel/nvme-core.o: kernel/drivers/nvme/core.c \
		kernel/drivers/nvme/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvme-completion.o: kernel/drivers/nvme/completion.c \
		kernel/drivers/nvme/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvme-timing.o: kernel/drivers/nvme/timing.c \
		kernel/drivers/nvme/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvme-pci.o: kernel/drivers/nvme/pci.c \
		kernel/drivers/nvme/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvme-io.o: kernel/drivers/nvme/io.c \
		kernel/drivers/nvme/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvme-queue.o: kernel/drivers/nvme/queue.c \
		kernel/drivers/nvme/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvme-admin.o: kernel/drivers/nvme/admin.c \
		kernel/drivers/nvme/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvme-namespace.o: kernel/drivers/nvme/namespace.c \
		kernel/drivers/nvme/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/nvme-self-test.o: kernel/drivers/nvme/self_test.c \
		kernel/drivers/nvme/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/e1000-core.o: kernel/drivers/net/core.c \
		kernel/drivers/net/internal.h kernel/drivers/net/core_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/e1000-protocol.o: kernel/drivers/net/protocol.c \
		kernel/drivers/net/internal.h kernel/drivers/net/core_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/e1000-self-test.o: kernel/drivers/net/self_test.c \
		kernel/drivers/net/internal.h kernel/drivers/net/core_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/e1000-runtime.o: kernel/drivers/net/runtime.c \
		kernel/drivers/net/internal.h kernel/drivers/net/core_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/e1000-rss.o: kernel/drivers/net/rss.c \
		kernel/drivers/net/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/e1000-pci.o: kernel/drivers/net/pci.c \
		kernel/drivers/net/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/e1000-queue.o: kernel/drivers/net/queue.c \
		kernel/drivers/net/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/e1000-recovery.o: kernel/drivers/net/recovery.c \
		kernel/drivers/net/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/rtl8126.o: kernel/drivers/net/rtl8126.c \
		kernel/drivers/net/internal.h kernel/drivers/net/core_internal.h \
		include/kernel/rtl8126.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci.o: kernel/drivers/usb/xhci/core.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-interrupt.o: kernel/drivers/usb/xhci/interrupt.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-runtime.o: kernel/drivers/usb/xhci/runtime.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-status.o: kernel/drivers/usb/xhci/status.c \
		kernel/drivers/usb/xhci/internal.h include/kernel/xhci.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-self-test.o: kernel/drivers/usb/xhci/self_test.c \
		kernel/drivers/usb/xhci/internal.h include/kernel/xhci.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-lifecycle.o: kernel/drivers/usb/xhci/lifecycle.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-topology.o: kernel/drivers/usb/xhci/topology.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-pci.o: kernel/drivers/usb/xhci/pci.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-ring.o: kernel/drivers/usb/xhci/ring.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-command.o: kernel/drivers/usb/xhci/command.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-command-runtime.o: kernel/drivers/usb/xhci/command_runtime.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-event.o: kernel/drivers/usb/xhci/event.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-event-runtime.o: kernel/drivers/usb/xhci/event_runtime.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-event-dispatch.o: kernel/drivers/usb/xhci/event_dispatch.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-transfer.o: kernel/drivers/usb/xhci/transfer.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-device.o: kernel/drivers/usb/xhci/device.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-device-lifecycle.o: kernel/drivers/usb/xhci/device_lifecycle.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-publication.o: kernel/drivers/usb/xhci/publication.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-root-runtime.o: kernel/drivers/usb/xhci/root_runtime.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-control-transfer.o: kernel/drivers/usb/xhci/control_transfer.c \
		kernel/drivers/usb/xhci/internal.h include/kernel/realtest.h | \
		$(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-enumeration.o: kernel/drivers/usb/xhci/enumeration.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-endpoint.o: kernel/drivers/usb/xhci/endpoint.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-hid.o: kernel/drivers/usb/xhci/hid.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-hid-runtime.o: kernel/drivers/usb/xhci/hid_runtime.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-audio.o: kernel/drivers/usb/xhci/audio.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-audio-runtime.o: kernel/drivers/usb/xhci/audio_runtime.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-msc.o: kernel/drivers/usb/xhci/msc.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-bluetooth.o: kernel/drivers/usb/xhci/bluetooth.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/usb-core.o: kernel/drivers/usb/core/device.c \
		include/usb/device.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/usb-hub.o: kernel/drivers/usb/xhci/hub.c \
		kernel/drivers/usb/xhci/internal.h include/usb/hub.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-hub-runtime.o: kernel/drivers/usb/xhci/hub_runtime.c \
		kernel/drivers/usb/xhci/internal.h include/usb/hub.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/xhci-hub-transfer.o: kernel/drivers/usb/xhci/hub_transfer.c \
		kernel/drivers/usb/xhci/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/usb-storage.o: kernel/drivers/usb/storage.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vfs.o: kernel/fs/vfs/core.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vfs-record-lock.o: kernel/fs/vfs/record_lock.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vfs-backend.o: kernel/fs/vfs/backend.c \
		kernel/fs/vfs/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vfs-file-io.o: kernel/fs/vfs/file_io.c \
		kernel/fs/vfs/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vfs-user-api.o: kernel/fs/vfs/user_api.c \
		kernel/fs/vfs/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/vfs-page-cache.o: kernel/fs/vfs/page_cache.c \
		kernel/fs/vfs/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/block.o: kernel/block/bio.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/block-core.o: kernel/block/queue.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/fat32.o: kernel/fs/nativefs/fat32.c kernel/fs/nativefs/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/fat32-directory-codec.o: kernel/fs/nativefs/directory_codec.c \
		kernel/fs/nativefs/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/fat32-table.o: kernel/fs/nativefs/fat_table.c \
		kernel/fs/nativefs/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/fat32-transaction.o: kernel/fs/nativefs/transaction.c \
		kernel/fs/nativefs/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/fat32-directory-lifecycle.o: kernel/fs/nativefs/directory_lifecycle.c \
		kernel/fs/nativefs/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/fat32-file-lifecycle.o: kernel/fs/nativefs/file_lifecycle.c \
		kernel/fs/nativefs/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/cache.o: kernel/fs/pagecache/cache.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/journal.o: kernel/fs/nativefs/journal.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/litefs.o: kernel/fs/nativefs/litefs.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/window-geometry.o: kernel/graphics/window_geometry.c \
		include/kernel/window_geometry.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-scene.o: kernel/graphics/scene.c \
		kernel/graphics/internal.h include/kernel/window_geometry.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-hit-test.o: kernel/graphics/hit_test.c \
		kernel/graphics/internal.h include/kernel/window_geometry.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-zorder.o: kernel/graphics/zorder.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-window.o: kernel/graphics/window.c \
		kernel/graphics/internal.h include/kernel/frame_trace.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-display.o: kernel/graphics/display.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-buffer.o: kernel/graphics/buffer.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-input.o: kernel/graphics/input.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-input-router.o: kernel/graphics/input_router.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-input-motion.o: kernel/graphics/input_motion.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-input-drag.o: kernel/graphics/input_drag.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-input-events.o: kernel/graphics/input_events.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-input-pump.o: kernel/graphics/input_pump.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-present.o: kernel/graphics/present.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-present-cursor.o: kernel/graphics/present_cursor.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-shell.o: kernel/graphics/shell.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-desktop-interaction.o: kernel/graphics/desktop_interaction.c \
        kernel/graphics/internal.h include/kernel/desktop_interaction.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-animation.o: kernel/graphics/animation.c \
        kernel/graphics/internal.h include/kernel/control_raster.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-assets.o: kernel/graphics/assets.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# 桌面和应用共享同一个界面解析器，内核版本仅替换分配器并排除用户文件接口。
$(BUILD)/kernel/graphics-ui-file.o: user/runtime/control_file.c user/runtime/control_file.h \
        include/uapi/control_tree.h include/uapi/control_validation.h include/uapi/window_theme.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -DUI_FILE_KERNEL -c $< -o $@

$(BUILD)/kernel/graphics-png.o: kernel/graphics/png.c \
		kernel/graphics/internal.h kernel/graphics/png.h \
		kernel/graphics/png_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-png-chunks.o: kernel/graphics/png_chunks.c \
		kernel/graphics/png_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-launcher.o: kernel/graphics/launcher.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-compositor.o: kernel/graphics/compositor.c \
	kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-publication-policy.o: kernel/graphics/publication_policy.c \
	kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-compositor-drag.o: kernel/graphics/compositor_drag.c \
	kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-render.o: kernel/graphics/render.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-control-tree.o: kernel/graphics/control_tree.c \
        include/kernel/control_tree.h include/uapi/control_tree.h include/uapi/window_theme.h include/kernel/rect.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-control-document.o: kernel/graphics/control_document.c \
        kernel/graphics/internal.h include/kernel/control_tree.h include/uapi/control_tree.h include/uapi/window_theme.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-caption-text.o: kernel/graphics/caption_text.c include/kernel/caption_text.h include/ascii_font.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-decorations.o: kernel/graphics/decorations.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-cursor-occlusion.o: kernel/graphics/cursor_occlusion.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-raster.o: kernel/graphics/raster.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-tile-metadata.o: kernel/graphics/tile_metadata.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-render-plan.o: kernel/graphics/render_plan.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-damage-plan.o: kernel/graphics/damage_plan.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-compose-cpu.o: kernel/graphics/compose_cpu.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-server.o: kernel/graphics/server.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/graphics-damage.o: kernel/graphics/damage.c \
		kernel/graphics/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/net-core.o: kernel/net/core/core.c kernel/net/core/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/net-firewall.o: kernel/net/core/firewall.c \
		kernel/net/core/internal.h include/kernel/net_core.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/net-self-test.o: kernel/net/core/self_test.c \
		kernel/net/core/internal.h include/kernel/net_core.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/net-manager.o: kernel/net/manager.c | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/socket.o: kernel/net/socket.c kernel/net/socket_internal.h \
		kernel/net/socket_model.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/socket-transport.o: kernel/net/socket_transport.c \
		kernel/net/socket_model.h include/kernel/socket.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/socket-io.o: kernel/net/socket_io.c \
		kernel/net/socket_internal.h kernel/net/socket_model.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/socket-protocol.o: kernel/net/socket_protocol.c \
		kernel/net/socket_model.h include/kernel/socket.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/kernel/socket-test.o: kernel/net/socket_test.c \
		kernel/net/socket_internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

loader: $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI

$(BUILD)/esp/EFI/BOOT/BOOTX64.EFI: $(LOADER_OBJECTS) | $(BUILD)/esp/EFI/BOOT
	$(LD) $(LOADER_LDFLAGS) $(LOADER_OBJECTS) -o $@

kernel: $(KERNEL_ELF)

$(KERNEL_PE): $(BUILD)/kernel/entry.o $(BUILD)/kernel/runtime-init.o \
                                    $(BUILD)/kernel/early-init.o \
                                    $(BUILD)/kernel/memory-init.o \
                                    $(BUILD)/kernel/devices-init.o \
                                    $(BUILD)/kernel/core-init.o \
                                    $(BUILD)/kernel/storage-init.o \
                                    $(BUILD)/kernel/scheduler-init.o \
                                    $(BUILD)/kernel/network-init.o \
                                    $(BUILD)/kernel/filesystem-init.o \
                                    $(BUILD)/kernel/filesystem-self-test.o \
                                    $(BUILD)/kernel/filesystem-root.o \
                                    $(BUILD)/kernel/graphics-init.o \
                                    $(BUILD)/kernel/userspace-init.o \
                                    $(BUILD)/kernel/init-self-tests.o \
                                    $(BUILD)/kernel/console.o $(BUILD)/kernel/console-backend.o \
                                    $(BUILD)/kernel/string.o \
                                    $(BUILD)/kernel/ascii_font.o \
                                    $(BUILD)/kernel/ascii-font-parser.o \
                                    $(BUILD)/kernel/paging.o \
                                    $(BUILD)/kernel/direct.o $(BUILD)/kernel/page-db.o \
                                    $(BUILD)/kernel/page-alloc.o $(BUILD)/kernel/percpu-page.o \
                                    $(BUILD)/kernel/mmu.o \
                                    $(BUILD)/kernel/page-table.o \
                                    $(BUILD)/kernel/kmalloc.o $(BUILD)/kernel/vmalloc.o \
                                    $(BUILD)/kernel/vm-space.o $(BUILD)/kernel/vm-object.o \
                                    $(BUILD)/kernel/vma.o \
                                    $(BUILD)/kernel/vm-map.o $(BUILD)/kernel/vm-protection.o \
                                    $(BUILD)/kernel/vm-shared.o $(BUILD)/kernel/vm-fault.o \
                                    $(BUILD)/kernel/vm-tlb.o \
                                    $(BUILD)/kernel/shared-section.o \
                                    $(BUILD)/kernel/cpu.o $(BUILD)/kernel/interrupt.o \
                                    $(BUILD)/kernel/interrupt-core.o \
                                    $(BUILD)/kernel/irq-core.o \
                                    $(BUILD)/kernel/uaccess-entry.o $(BUILD)/kernel/uaccess.o \
                                    $(BUILD)/kernel/syscall.o $(BUILD)/kernel/syscall-core.o \
                                    $(BUILD)/kernel/linux-abi.o \
                                    $(BUILD)/kernel/linux-phase7-test.o \
                                    $(BUILD)/kernel/linux-phase8-test.o \
                                    $(PHASE13_KERNEL_OBJECT) \
                                    $(BUILD)/kernel/syscall-debug.o \
                                    $(BUILD)/kernel/syscall-network.o \
                                    $(BUILD)/kernel/syscall-graphics.o \
                                    $(BUILD)/kernel/syscall-font.o \
                                    $(BUILD)/kernel/syscall-image.o \
                                    $(BUILD)/kernel/syscall-clipboard.o \
                                    $(BUILD)/kernel/syscall-filesystem.o \
                                    $(BUILD)/kernel/syscall-pipe.o \
                                    $(BUILD)/kernel/syscall-audio.o \
                                    $(BUILD)/kernel/syscall-device.o \
                                    $(BUILD)/kernel/syscall-sync.o \
                                    $(BUILD)/kernel/syscall-random.o \
                                    $(BUILD)/kernel/syscall-io.o \
                                    $(BUILD)/kernel/syscall-process.o \
                                    $(BUILD)/kernel/syscall-vm.o \
                                    $(BUILD)/kernel/syscall-handles.o \
                                    $(BUILD)/kernel/context.o \
                                    $(BUILD)/kernel/user-entry.o \
                                    $(BUILD)/kernel/apic.o $(BUILD)/kernel/acpi.o \
                                    $(BUILD)/kernel/smp.o $(BUILD)/kernel/trampoline.o \
                                    $(BUILD)/kernel/tlb.o \
                                    $(BUILD)/kernel/timer.o \
                                    $(BUILD)/kernel/stack-check.o \
                                    $(BUILD)/kernel/debug-stage.o \
                                    $(BUILD)/kernel/realtest.o \
                                    $(BUILD)/kernel/reboot.o \
                                     $(BUILD)/kernel/sched-core.o \
                                    $(BUILD)/kernel/sched-clock.o \
                                    $(BUILD)/kernel/sched-monitor.o \
                                    $(BUILD)/kernel/sched-runqueue.o \
                                    $(BUILD)/kernel/sched-balance.o \
                                    $(BUILD)/kernel/process-core.o \
                                    $(BUILD)/kernel/process-thread.o \
                                    $(BUILD)/kernel/process-exit.o \
                                    $(BUILD)/kernel/resource.o \
                                    $(BUILD)/kernel/user-elf.o \
                                    $(BUILD)/kernel/user-runtime-test.o \
                                    $(BUILD)/kernel/user-services-init.o \
                                    $(BUILD)/kernel/user-test-blob.o \
                                    $(BUILD)/kernel/object-core.o $(BUILD)/kernel/handle.o \
                                    $(BUILD)/kernel/wait.o $(BUILD)/kernel/futex.o \
                                    $(BUILD)/kernel/mutex.o \
                                    $(BUILD)/kernel/io-mutex.o \
                                     $(BUILD)/kernel/canonical-io.o \
                                    $(BUILD)/kernel/completion-port.o \
                                    $(BUILD)/kernel/message-port.o \
                                    $(BUILD)/kernel/pipe.o \
                                    $(BUILD)/kernel/timer-core.o \
                                    $(BUILD)/kernel/deferred.o $(BUILD)/kernel/background.o $(BUILD)/kernel/service.o \
                                    $(BUILD)/kernel/watchdog.o \
                                    $(BUILD)/kernel/power.o \
                                    $(BUILD)/kernel/crash-dump.o \
                                    $(BUILD)/kernel/input-core.o \
                                    $(BUILD)/kernel/gpu-core.o $(BUILD)/kernel/display-core.o \
                                    $(BUILD)/kernel/dma-core.o $(BUILD)/kernel/audio-core.o \
                                    $(BUILD)/kernel/hda.o \
                                    $(BUILD)/kernel/bluetooth-core.o \
                                    $(BUILD)/kernel/package.o \
                                    $(BUILD)/kernel/firmware-core.o \
                                    $(BUILD)/kernel/rcu.o \
                                    $(BUILD)/kernel/telemetry.o \
                                    $(BUILD)/kernel/perf.o \
                                    $(BUILD)/kernel/iommu-core.o \
                                    $(BUILD)/kernel/device-core.o \
                                    $(BUILD)/kernel/pci-core.o \
                                    $(NVIDIA_PORT_OBJECTS) \
                                    $(NVIDIA_RM_DRIVER_OBJECTS) \
                                    $(BUILD)/kernel/nvidia-rm-frontend.o \
                                     $(BUILD)/kernel/nvme-core.o $(BUILD)/kernel/nvme-completion.o \
                                     $(BUILD)/kernel/nvme-timing.o \
                                     $(BUILD)/kernel/nvme-pci.o \
                                     $(BUILD)/kernel/nvme-io.o \
                                     $(BUILD)/kernel/nvme-queue.o \
                                    $(BUILD)/kernel/nvme-admin.o \
                                    $(BUILD)/kernel/nvme-namespace.o \
                                    $(BUILD)/kernel/nvme-self-test.o \
                                    $(BUILD)/kernel/e1000-core.o \
                                    $(BUILD)/kernel/e1000-protocol.o \
                                    $(BUILD)/kernel/e1000-self-test.o \
                                    $(BUILD)/kernel/e1000-runtime.o \
                                    $(BUILD)/kernel/e1000-rss.o \
                                    $(BUILD)/kernel/e1000-pci.o \
                                    $(BUILD)/kernel/e1000-queue.o \
                                    $(BUILD)/kernel/e1000-recovery.o \
                                    $(BUILD)/kernel/rtl8126.o \
                                     $(BUILD)/kernel/usb-core.o $(BUILD)/kernel/usb-hub.o $(BUILD)/kernel/xhci-hub-runtime.o $(BUILD)/kernel/xhci-hub-transfer.o $(BUILD)/kernel/usb-storage.o \
                                     $(BUILD)/kernel/xhci.o $(BUILD)/kernel/xhci-interrupt.o $(BUILD)/kernel/xhci-runtime.o $(BUILD)/kernel/xhci-status.o $(BUILD)/kernel/xhci-self-test.o \
                                     $(BUILD)/kernel/xhci-lifecycle.o \
                                     $(BUILD)/kernel/xhci-topology.o \
                                    $(BUILD)/kernel/xhci-ring.o \
                                    $(BUILD)/kernel/xhci-pci.o \
                                    $(BUILD)/kernel/xhci-command.o \
                                    $(BUILD)/kernel/xhci-command-runtime.o \
                                    $(BUILD)/kernel/xhci-event.o \
                                    $(BUILD)/kernel/xhci-event-runtime.o \
                                    $(BUILD)/kernel/xhci-event-dispatch.o \
                                    $(BUILD)/kernel/xhci-transfer.o \
                                    $(BUILD)/kernel/xhci-device.o \
                                    $(BUILD)/kernel/xhci-device-lifecycle.o \
                                    $(BUILD)/kernel/xhci-publication.o \
                                    $(BUILD)/kernel/xhci-root-runtime.o \
                                    $(BUILD)/kernel/xhci-control-transfer.o \
                                    $(BUILD)/kernel/xhci-enumeration.o \
                                    $(BUILD)/kernel/xhci-endpoint.o \
                                    $(BUILD)/kernel/xhci-hid.o \
                                    $(BUILD)/kernel/xhci-hid-runtime.o \
                                    $(BUILD)/kernel/xhci-audio.o \
                                    $(BUILD)/kernel/xhci-audio-runtime.o \
                                    $(BUILD)/kernel/xhci-msc.o \
                                    $(BUILD)/kernel/xhci-bluetooth.o \
                                     $(BUILD)/kernel/vfs.o $(BUILD)/kernel/vfs-backend.o \
                                     $(BUILD)/kernel/vfs-record-lock.o \
                                     $(BUILD)/kernel/vfs-file-io.o \
                                     $(BUILD)/kernel/vfs-user-api.o \
                                     $(BUILD)/kernel/vfs-page-cache.o \
                                     $(BUILD)/kernel/block.o \
                                    $(BUILD)/kernel/block-core.o \
                                    $(BUILD)/kernel/fat32.o $(BUILD)/kernel/fat32-directory-codec.o $(BUILD)/kernel/fat32-table.o $(BUILD)/kernel/fat32-transaction.o $(BUILD)/kernel/fat32-directory-lifecycle.o $(BUILD)/kernel/fat32-file-lifecycle.o $(BUILD)/kernel/cache.o \
                                    $(BUILD)/kernel/journal.o \
                                    $(BUILD)/kernel/litefs.o \
                                    $(BUILD)/kernel/window-geometry.o \
                                    $(BUILD)/kernel/graphics-scene.o \
                                    $(BUILD)/kernel/graphics-hit-test.o \
                                    $(BUILD)/kernel/graphics-zorder.o \
                                    $(BUILD)/kernel/graphics-window.o \
                                    $(BUILD)/kernel/graphics-display.o \
                                    $(BUILD)/kernel/graphics-buffer.o \
                                    $(BUILD)/kernel/graphics-input.o \
                                    $(BUILD)/kernel/graphics-input-router.o \
                                    $(BUILD)/kernel/graphics-input-motion.o \
                                    $(BUILD)/kernel/graphics-input-drag.o \
                                    $(BUILD)/kernel/graphics-input-events.o \
                                    $(BUILD)/kernel/graphics-input-pump.o \
                                    $(BUILD)/kernel/graphics-present.o \
                                    $(BUILD)/kernel/graphics-present-cursor.o \
                                    $(BUILD)/kernel/graphics-shell.o \
                                    $(BUILD)/kernel/graphics-desktop-interaction.o \
                                    $(BUILD)/kernel/graphics-animation.o \
                                    $(BUILD)/kernel/graphics-assets.o \
                                    $(BUILD)/kernel/graphics-ui-file.o \
                                    $(BUILD)/kernel/graphics-png.o \
                                    $(BUILD)/kernel/graphics-png-chunks.o \
                                    $(BUILD)/kernel/graphics-launcher.o \
                                    $(BUILD)/kernel/graphics-compositor.o \
                                    $(BUILD)/kernel/graphics-publication-policy.o \
                                    $(BUILD)/kernel/graphics-compositor-drag.o \
                                    $(BUILD)/kernel/graphics-render.o \
                                    $(BUILD)/kernel/graphics-control-tree.o \
                                    $(BUILD)/kernel/graphics-control-document.o \
                                    $(BUILD)/kernel/graphics-caption-text.o \
                                    $(BUILD)/kernel/graphics-decorations.o \
                                    $(BUILD)/kernel/graphics-cursor-occlusion.o \
                                    $(BUILD)/kernel/graphics-raster.o \
                                    $(BUILD)/kernel/graphics-tile-metadata.o \
                                    $(BUILD)/kernel/graphics-render-plan.o \
                                    $(BUILD)/kernel/graphics-damage-plan.o \
                                     $(BUILD)/kernel/graphics-compose-cpu.o \
                                     $(BUILD)/kernel/graphics-server.o \
                                     $(BUILD)/kernel/graphics-damage.o \
                                    $(BUILD)/kernel/net-core.o \
                                    $(BUILD)/kernel/net-firewall.o \
                                    $(BUILD)/kernel/net-self-test.o \
                                    $(BUILD)/kernel/net-manager.o \
                                    $(BUILD)/kernel/socket.o \
                                    $(BUILD)/kernel/socket-transport.o \
                                    $(BUILD)/kernel/socket-io.o \
                                    $(BUILD)/kernel/socket-protocol.o \
                                    $(BUILD)/kernel/socket-test.o | \
                                    $(BUILD)/kernel
		$(KERNEL_LINK) $^ -o $@

$(KERNEL_ELF): $(KERNEL_PE) | $(BUILD)/esp/EFI/OS
	$(OBJCOPY) -O elf64-x86-64 $< $@

$(BUILD)/.host-toolchain: FORCE | $(BUILD)
	@{ printf '%s\n' '$(HOSTCC)' '$(OS)' '-O2 -Wall -Wextra -Werror -Iinclude'; \
		$(HOSTCC) -dumpmachine && $(HOSTCC) --version; } > $@.tmp
	@if ! cmp -s $@.tmp $@ 2>/dev/null; then mv $@.tmp $@; else rm -f $@.tmp; fi

$(BUILD)/build-id.exe: tools/build_id.c boot/uefi/sha256.c $(BUILD)/.host-toolchain makefile tools/host-tests.mk | $(BUILD)
	$(call HOST_TEST_LINK,-O2 -Wall -Wextra -Werror -Iinclude)

$(KERNEL_BUILD_ID): $(KERNEL_ELF) $(BUILD)/build-id.exe | $(BUILD)/esp/EFI/OS
	$(BUILD)/build-id.exe $(KERNEL_ELF) $@

$(KERNEL_SYMBOLS): $(KERNEL_ELF) | $(BUILD)/kernel
	$(OBJDUMP) -t $< > $@

OS_CMDLINE ?= console=framebuffer loglevel=info
ifeq ($(OS_REALTEST),1)
OS_CMDLINE = realtest=1 reboot=failure
endif

$(BUILD)/esp/EFI/OS/loader.conf: makefile $(DEBUG_SERIAL_STAMP) | $(BUILD)/esp/EFI/OS
	@printf '%s\n' 'kernel=\EFI\OS\kernel.elf' \
		'cmdline=$(OS_CMDLINE)' > $@

INIT_SERVICE_OFFSET = $(shell $(OBJDUMP) -t $(BUILD)/kernel/user-init-blob.o 2>/dev/null | \
	awk '$$NF == "user_init_service" { for (i=1;i<=NF;i++) if ($$i == ".rdata") { print "0x" $$1; exit } print $$9; exit }')

$(BUILD)/esp/EFI/OS/init: $(BUILD)/kernel/user-init-blob.bin $(BUILD)/make-init-image.exe | $(BUILD)/esp/EFI/OS
	$(BUILD)/make-init-image.exe $< $@ 0 $(INIT_SERVICE_OFFSET)

$(BUILD)/esp/EFI/OS/init-runtime: $(BUILD)/kernel/user-init-blob.bin $(BUILD)/make-init-image.exe | $(BUILD)/esp/EFI/OS
	$(BUILD)/make-init-image.exe $< $@ 1 $(INIT_SERVICE_OFFSET)

$(BUILD)/esp/sbin/deviced: $(BUILD)/esp/EFI/OS/init | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/sbin/logd: $(BUILD)/esp/EFI/OS/init | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/sbin/crashd: $(BUILD)/esp/EFI/OS/init | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/lib/ld-system.so.1: $(DYNAMIC_LOADER_ELF) | $(BUILD)/esp/lib
	cp $< $@

$(BUILD)/esp/lib/ld-linux-system.so.1: $(BUILD)/esp/lib/ld-system.so.1 | $(BUILD)/esp/lib
	cp $< $@

$(BUILD)/esp/lib/libgraphics.so.1: $(DYNAMIC_GFX_LIBRARY_ELF) | $(BUILD)/esp/lib
	cp $< $@

$(BUILD)/esp/lib/libsystemc.so.1: $(DYNAMIC_LIBC_ELF) | $(BUILD)/esp/lib
	cp $< $@

$(BUILD)/esp/usr/apps/dyn-gfx/dyn-gfx: $(DYNAMIC_GFX_TEST_ELF) | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/libc-test/libc-test: $(DYNAMIC_LIBC_TEST_ELF) | $(BUILD)/esp
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/linux-events-test/linux-events-test: $(DYNAMIC_LINUX_EVENTS_TEST_ELF) | $(BUILD)/esp
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/fat-fsync-probe/fat-fsync-probe: $(DYNAMIC_FSYNC_PROBE_ELF) | $(BUILD)/esp
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/imageview/imageview: $(DYNAMIC_IMAGEVIEW_ELF) | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/nasm/nasm: $(DYNAMIC_NASM_ELF) | $(BUILD)/esp
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/ndisasm/ndisasm: $(DYNAMIC_NDISASM_ELF) | $(BUILD)/esp
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/gshell/gshell: $(DYNAMIC_GSHELL_ELF) | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/chrome/chrome-start: $(DYNAMIC_CHROME_START_ELF) | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/notepad/notepad: $(DYNAMIC_NOTEPAD_ELF) | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/fileman/fileman: $(DYNAMIC_FILEMAN_ELF) | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/usr/apps/window-frame-probe/window-frame-probe: tests/user/window_frame_probe.c \
        include/uapi/linux_abi.h include/uapi/window.h include/uapi/window_state.h | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	$(ELFCC) $(ELF_APP_CFLAGS) -fPIC -nostdlib -pie -Wl,-e,_start \
		-Wl,--hash-style=sysv -Wl,--dynamic-linker,/lib/ld-linux-system.so.1 $< -o $@

$(BUILD)/esp/usr/apps/control-tree-demo/control-tree-demo: tests/user/control_tree_demo.c \
        include/uapi/linux_abi.h include/uapi/window.h include/uapi/window_state.h include/uapi/control_tree.h include/uapi/window_theme.h include/uapi/syscall.h | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	$(ELFCC) $(ELF_APP_CFLAGS) -fPIC -nostdlib -pie -Wl,-e,_start \
		-Wl,--hash-style=sysv -Wl,--dynamic-linker,/lib/ld-linux-system.so.1 $< -o $@

$(BUILD)/esp/usr/apps/taskmgr/taskmgr: $(DYNAMIC_TASKMGR_ELF) | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/sbin/audiod: $(USER_AUDIOD_ELF) | $(BUILD)/esp/sbin
	cp $< $@

$(BUILD)/esp/usr/apps/settings/settings: $(DYNAMIC_SETTINGS_ELF) | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@


# 命令行应用也使用自己的应用包目录，由终端按应用名查找。
$(BUILD)/esp/usr/apps/wget/wget: $(USER_WGET_ELF) | $(BUILD)/esp
	mkdir -p $(@D)
	cp $< $@

$(BUILD)/esp/sbin/netd: $(USER_NETD_ELF) | $(BUILD)/esp/sbin
	cp $< $@

ifeq ($(OS_PHASE13),1)
PHASE13_TEST_IMAGE = $(BUILD)/esp/usr/apps/linux-events-test/linux-events-test
endif

# Refresh diagnostic images already installed by an explicit test build;
# fresh desktop images do not acquire extra test applications.
# 兼容旧诊断副本：先在新归属重建，清理脚本随后才允许删除旧文件。
EXISTING_DIAGNOSTIC_IMAGES = $(foreach app,fat-fsync-probe linux-events-test,\
    $(if $(wildcard $(BUILD)/esp/sbin/$(app) $(BUILD)/esp/usr/apps/$(app)/$(app)),\
    $(BUILD)/esp/usr/apps/$(app)/$(app)))

.PHONY: rtl8126-payload
rtl8126-payload:
	python3 tools/stage-rtl8126-firmware.py --build "$(BUILD)"

# 仅迁移历史手工部署的诊断程序；已经由新暂存流程发布的程序绝不覆盖。
LEGACY_DIAGNOSTIC_NAMES = taskmgr-cpu-probe tcp-wake-probe chrome-glibc-probe vulkan-probe vulkan-video
LEGACY_DIAGNOSTIC_SOURCES = $(foreach app,$(LEGACY_DIAGNOSTIC_NAMES),$(wildcard $(BUILD)/esp/sbin/$(app)))
.PHONY: legacy-diagnostic-apps
legacy-diagnostic-apps:
	@set -e; $(foreach source,$(LEGACY_DIAGNOSTIC_SOURCES),\
	    target="$(BUILD)/esp/usr/apps/$(notdir $(source))/$(notdir $(source))"; \
	    if [ ! -e "$$target" ] && [ ! -L "$$target" ]; then \
	        mkdir -p "$$(dirname "$$target")"; cp "$(source)" "$$target"; \
	    fi;) :

# 供协作暂存单独调用，只准备应用负载，不触发完整 ESP、镜像或虚拟机。
BUILTIN_APP_NAMES = gshell notepad fileman settings taskmgr imageview dyn-gfx \
    nasm ndisasm wget libc-test linux-events-test fat-fsync-probe ui-file-demo \
    control-tree-demo window-frame-probe audio-test
.PHONY: builtin-apps clean-runtime-aliases
builtin-apps: $(foreach app,$(BUILTIN_APP_NAMES),$(BUILD)/esp/usr/apps/$(app)/$(app)) \
    $(DESKTOP_ASSETS) $(VFS_SELF_TEST_SEED) legacy-diagnostic-apps \
    $(BUILD)/esp/usr/apps/settings/wallpaper $(BUILD)/esp/usr/apps/settings/file-picker \
    $(BUILD)/esp/usr/apps/ui-file-demo/controls.ui

# 新应用和 Chrome 暂存完成后再调用；内部仍逐文件验证新归属。
clean-runtime-aliases:
	python3 tools/clean-runtime-aliases.py "$(BUILD)/esp"

esp: rtl8126-payload legacy-diagnostic-apps $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI $(KERNEL_ELF) \
     $(BUILD)/esp/EFI/OS/loader.conf $(KERNEL_BUILD_ID) $(KERNEL_SYMBOLS) \
     $(BUILD)/esp/tmp $(BUILD)/esp/log \
     $(DESKTOP_ASSETS) \
     $(VFS_SELF_TEST_SEED) \
     $(BUILD)/esp/EFI/OS/init $(BUILD)/esp/EFI/OS/init-runtime \
     $(BUILD)/esp/lib/ld-system.so.1 $(BUILD)/esp/lib/ld-linux-system.so.1 \
     $(BUILD)/esp/lib/libsystemc.so.1 \
     $(BUILD)/esp/lib/libgraphics.so.1 \
     $(BUILD)/esp/usr/apps/dyn-gfx/dyn-gfx $(BUILD)/esp/usr/apps/libc-test/libc-test \
     $(BUILD)/esp/usr/apps/imageview/imageview $(BUILD)/esp/usr/apps/nasm/nasm \
     $(BUILD)/esp/usr/apps/ndisasm/ndisasm \
     $(BUILD)/esp/sbin/deviced $(BUILD)/esp/sbin/logd \
     $(BUILD)/esp/sbin/crashd $(BUILD)/esp/usr/apps/gshell/gshell \
     $(BUILD)/esp/usr/apps/chrome/chrome-start \
     $(BUILD)/esp/usr/apps/notepad/notepad $(BUILD)/esp/usr/apps/fileman/fileman \
     $(BUILD)/esp/usr/apps/ui-file-demo/ui-file-demo $(BUILD)/esp/usr/apps/ui-file-demo/controls.ui \
     $(BUILD)/esp/usr/apps/taskmgr/taskmgr \
     $(BUILD)/esp/sbin/audiod \
     $(BUILD)/esp/usr/apps/settings/settings $(BUILD)/esp/sbin/netd \
     $(BUILD)/esp/usr/apps/wget/wget $(PHASE13_TEST_IMAGE) $(EXISTING_DIAGNOSTIC_IMAGES)
	@python3 tools/migrate-system-layout.py "$(BUILD)/esp"
	@python3 tools/clean-runtime-aliases.py "$(BUILD)/esp"
	@echo ESP image prepared at $(BUILD)/esp

release-metadata: $(KERNEL_BUILD_ID) $(KERNEL_SYMBOLS)
	@echo Release metadata prepared: $(KERNEL_BUILD_ID) and $(KERNEL_SYMBOLS)

include tools/host-tests.mk

$(BUILD)/libc-header-sanity.o: tools/libc_header_sanity.c | $(BUILD)
	$(ELFCC) $(ELF_COMMON_CFLAGS) -c $< -o $@

libc-header-sanity: $(BUILD)/libc-header-sanity.o
	@echo libc header sanity passed

dynamic-loader-sanity: $(DYNAMIC_LOADER_ELF) $(DYNAMIC_GFX_LIBRARY_ELF) \
                       $(DYNAMIC_GFX_TEST_ELF)

ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/verify-dynamic-loader.ps1 \
		-Loader "$(DYNAMIC_LOADER_ELF)" -Library "$(DYNAMIC_GFX_LIBRARY_ELF)" \
		-Test "$(DYNAMIC_GFX_TEST_ELF)" -Objdump "$(ELFOBJDUMP)"
else
	@ELFOBJDUMP="$(ELFOBJDUMP)" bash tools/verify-dynamic-loader.sh "$(DYNAMIC_LOADER_ELF)" \
		"$(DYNAMIC_GFX_LIBRARY_ELF)" "$(DYNAMIC_GFX_TEST_ELF)"
endif

libc-sanity: libc-header-sanity $(DYNAMIC_LOADER_ELF) $(DYNAMIC_LIBC_ELF) $(DYNAMIC_LIBC_TEST_ELF)

ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/verify-libc.ps1 \
		-Loader "$(DYNAMIC_LOADER_ELF)" -Library "$(DYNAMIC_LIBC_ELF)" \
		-Test "$(DYNAMIC_LIBC_TEST_ELF)" -Objdump "$(ELFOBJDUMP)"
else
	@bash tools/verify-libc.sh "$(DYNAMIC_LOADER_ELF)" \
		"$(DYNAMIC_LIBC_ELF)" "$(DYNAMIC_LIBC_TEST_ELF)" "$(ELFOBJDUMP)"
endif


clean:
	rm -rf $(BUILD)

# Invalidate existing ELF objects made before the wide-character ABI guard
# existed. Newly compiled objects also track it through their .d files.
$(wildcard $(ELF_RUNTIME_DIR)/*.o): user/runtime/elf_abi.h

# 让头文件修改自动使相关 Loader/Kernel 对象失效，避免 ABI 结构体错位。
-include $(wildcard $(BUILD)/loader/*.d) $(wildcard $(BUILD)/kernel/*.d) \
         $(wildcard $(BUILD)/*.exe.d) \
         $(wildcard $(BUILD)/nvidia/host-test/*.o.d) \
         $(wildcard $(BUILD)/user/*.d) $(wildcard $(BUILD)/elf/*.d) \
         $(wildcard $(BUILD)/nasm/*.d) $(NASM_DEPENDENCY_FILES) \
         $(OPENSSL_DEPENDENCY_FILES) $(BLEND2D_DEPENDENCY_FILES)

$(BUILD)/kernel/syscall-clipboard.o: kernel/syscall/clipboard.c include/uapi/clipboard.h include/kernel/clipboard.h kernel/syscall/internal.h | $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(ELF_RUNTIME_DIR)/notepad.o $(ELF_RUNTIME_DIR)/gshell.o: user/runtime/clipboard.h include/uapi/clipboard.h

$(ELF_RUNTIME_DIR)/settings.o $(ELF_RUNTIME_DIR)/gshell.o $(ELF_RUNTIME_DIR)/notepad.o $(ELF_RUNTIME_DIR)/fileman.o $(ELF_RUNTIME_DIR)/taskmgr.o $(ELF_RUNTIME_DIR)/imageview.o: user/runtime/caption_tree.h

$(eval $(call DYNAMIC_TEXT_APP_RULE,wallpaper,user/desktop/wallpaper/main.c))
$(ELF_RUNTIME_DIR)/wallpaper.o: user/runtime/file_picker.h user/runtime/modal_tree.h user/runtime/caption_tree.h include/uapi/desktop.h
$(BUILD)/esp/usr/apps/settings/wallpaper: $(ELF_RUNTIME_DIR)/wallpaper.elf | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@
esp: $(BUILD)/esp/usr/apps/settings/wallpaper

# 浏览器复用系统文件选择框；两个入口共享同一份实现。
$(BUILD)/esp/usr/apps/settings/file-picker: $(ELF_RUNTIME_DIR)/wallpaper.elf | $(BUILD)/esp/sbin
	mkdir -p $(@D)
	cp $< $@
$(BUILD)/esp/Downloads:
	mkdir -p $@
esp: $(BUILD)/esp/usr/apps/settings/file-picker $(BUILD)/esp/Downloads

$(BUILD)/esp/usr/apps/chrome/download-test.html: tests/browser/download.html
	mkdir -p $(@D)
	cp $< $@
esp: $(BUILD)/esp/usr/apps/chrome/download-test.html

# 两种物理音频后端共用同一播放验证程序。
$(eval $(call DYNAMIC_TEXT_APP_RULE,audio-test,user/diagnostics/audio-test/main.c))
$(ELF_RUNTIME_DIR)/audio-test.o: include/uapi/audio.h
$(BUILD)/esp/usr/apps/audio-test/audio-test: $(ELF_RUNTIME_DIR)/audio-test.elf
	mkdir -p $(@D)
	cp $< $@
esp: $(BUILD)/esp/usr/apps/audio-test/audio-test
