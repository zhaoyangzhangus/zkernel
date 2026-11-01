CLANG ?= clang
LLD ?= lld-link
CFLAGS = -target x86_64-pc-windows-msvc -O3 -std=c11 -ffreestanding -fno-builtin \
         -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
         -fno-exceptions -fno-strict-aliasing -mno-red-zone -Wall -Wextra -Werror \
         -DFM_UEFI -I.
OBJECTS = build/frame_pool.obj build/uefi_main.obj
EFI = esp/EFI/BOOT/BOOTX64.EFI
all: $(EFI)
build:
	mkdir -p build
esp/EFI/BOOT:
	mkdir -p esp/EFI/BOOT
build/%.obj: %.c frame_pool.h uefi_shim.h uefi_min.h | build
	$(CLANG) $(CFLAGS) -c $< -o $@
$(EFI): $(OBJECTS) | esp/EFI/BOOT
	$(LLD) /nologo /subsystem:efi_application /entry:efi_main /machine:x64 /nodefaultlib /out:$@ $(OBJECTS)
.PHONY: all clean
clean:
	rm -rf build esp/EFI/BOOT/BOOTX64.EFI
# Same allocator code, compiled in freestanding adapter mode, smoke-tested
# using Linux mmap as a simulator (not counted as a QEMU bare-metal run).
.PHONY: host-smoke
host-smoke: | build
	gcc -O3 -DFM_UEFI -std=c11 -Wall -Wextra -Werror -o build/host_smoke frame_pool.c host_smoke.c
	./build/host_smoke
.PHONY: host-regression host-asan
host-regression:
	gcc -O2 -g -std=c11 -Wall -Wextra -Werror -pedantic -o build/frame_test_native \
	    frame_pool.c sim_host.c persist.c test_frame_pool.c -pthread
	./build/frame_test_native
host-asan:
	gcc -O1 -g -std=c11 -Wall -Wextra -Werror -pedantic \
	    -fno-omit-frame-pointer -fsanitize=address,undefined \
	    -o build/frame_test_asan frame_pool.c sim_host.c persist.c test_frame_pool.c -pthread
	./build/frame_test_asan

.PHONY: host-smoke-asan
host-smoke-asan: | build
	gcc -O1 -g -DFM_UEFI -std=c11 -Wall -Wextra -Werror \
	    -fno-omit-frame-pointer -fsanitize=address,undefined \
	    -o build/host_smoke_asan frame_pool.c host_smoke.c
	./build/host_smoke_asan
