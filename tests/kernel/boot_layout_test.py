"""Freeze the loader/kernel wire layout while internal names are refactored."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
for name in ('include/kernel/bootinfo.h', 'include/boot/elf.h',
             'include/boot/pe.h', 'include/kernel/mm_boot.h',
             'include/kernel/memory_map.h'):
    text = (ROOT / name).read_text()
    assert ('LITE' + 'OS_') not in text, name
    assert ('lite' + 'os_') not in text, name
for header in (ROOT / 'include/kernel').glob('init_*.h'):
    assert ('lite' + 'os_init_') not in header.read_text(), header
assembly = (ROOT / 'kernel/process/init_blob.S').read_text()
make = (ROOT / 'makefile').read_text()
assert '.globl user_init_service' in assembly
assert '\nuser_init_service:' in assembly
assert '$$NF == "user_init_service"' in make
# 这些偏移量在命名空间迁移前测量，属于 ABI；不是根据被测结构重新生成的预期值。
OFFSETS = {
    'Magic': 0, 'Version': 8, 'Size': 12, 'Flags': 16,
    'KernelPhysicalBase': 24, 'KernelVirtualBase': 32, 'KernelSize': 40,
    'KernelEntry': 48, 'MemoryMap': 56, 'MemoryMapSize': 64,
    'MemoryDescriptorSize': 72, 'MemoryDescriptorVersion': 80, 'Reserved0': 84,
    'FrameBufferBase': 88, 'FrameBufferSize': 96, 'FrameBufferWidth': 104,
    'FrameBufferHeight': 108, 'FrameBufferPixelsPerScanLine': 112,
    'FrameBufferFormat': 116, 'FrameBufferMask': 120, 'Reserved1': 136,
    'AcpiRsdp': 144, 'Smbios': 152, 'Smbios3': 160, 'RuntimeServices': 168,
    'SystemTable': 176, 'RandomSeed': 184, 'CommandLine': 216,
    'LoaderName': 224, 'LoaderNameSize': 232, 'MemoryMapBufferSize': 240,
    'CommandLineSize': 248, 'BootInfoPhysicalBase': 256,
    'BootstrapStackBase': 264, 'BootstrapStackSize': 272,
    'BootstrapStackTop': 280, 'LoaderImageBase': 288, 'LoaderImageSize': 296,
    'PageDatabasePhysicalBase': 304, 'PageDatabaseSize': 312,
    'ApTrampolineBase': 320, 'ApTrampolineSize': 328, 'BootDeviceHandle': 336,
    'BootDevicePathSize': 344, 'BootDevicePartitionNumber': 348,
    'BootDevicePartitionStartLba': 352, 'BootDevicePartitionSizeLba': 360,
    'BootDevicePartitionSignature': 368, 'BootDevicePathHash': 384,
    'BootDevicePartitionMbrType': 416, 'BootDevicePartitionSignatureType': 417,
    'BootDeviceTransport': 418, 'Reserved3': 419, 'Reserved4': 420,
}
code = '''
#include <stddef.h>
#include <stdio.h>
#include <kernel/bootinfo.h>
#include <boot/elf.h>
#include <boot/pe.h>
#define CHECK(c) _Static_assert((c), #c)
CHECK(sizeof(OS_BOOT_INFO) == 424);
CHECK(OS_BOOTINFO_MAGIC == 0x4C4954454F534249ULL);
CHECK(OS_BOOTINFO_VERSION == 6);
CHECK(OS_BOOTSTRAP_STACK_SIZE == 2ULL * 1024ULL * 1024ULL);
CHECK(OS_BOOTINFO_HAS_FRAMEBUFFER == 1);
CHECK(OS_BOOTINFO_HAS_BOOT_DEVICE == (1U << 11));
CHECK(OS_BOOT_DEVICE_TRANSPORT_USB == 1);
CHECK(OS_BOOT_DEVICE_TRANSPORT_NVME == 2);
CHECK(OS_PIXEL_RGBR == 0 && OS_PIXEL_BGRR == 1);
CHECK(OS_PIXEL_BITMASK == 2 && OS_PIXEL_BLT_ONLY == 3);
CHECK(sizeof(OS_ELF64_HEADER) == 64);
CHECK(sizeof(OS_ELF64_PROGRAM_HEADER) == 56);
CHECK(sizeof(OS_ELF_IMAGE) == 48);
CHECK(sizeof(OS_PE_IMAGE) == 32);
CHECK(offsetof(OS_ELF64_HEADER, Entry) == 24);
CHECK(offsetof(OS_ELF64_HEADER, ProgramHeaderOffset) == 32);
CHECK(offsetof(OS_ELF64_PROGRAM_HEADER, Offset) == 8);
CHECK(offsetof(OS_ELF64_PROGRAM_HEADER, FileSize) == 32);
CHECK(offsetof(OS_ELF64_PROGRAM_HEADER, Alignment) == 48);
'''
for field, offset in OFFSETS.items():
    code += f'CHECK(offsetof(OS_BOOT_INFO, {field}) == {offset});\n'
code += 'int main(void) { puts("Boot ABI: 54 field offsets, image layouts and protocol constants unchanged"); }\n'
with tempfile.TemporaryDirectory(prefix='boot-layout-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'include'), '-x', 'c', '-', '-o', binary],
                   input=code, text=True, check=True)
    subprocess.run([binary], check=True)
