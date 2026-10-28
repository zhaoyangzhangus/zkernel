"""Native startup renaming retains machine code and Linux ABI values."""
from pathlib import Path
import hashlib
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
make = (ROOT / 'makefile').read_text()
assert 'liteos_crt_entry' not in make
assert '-e libc_crt_entry' in make
assert 'audiod-blob.o' not in make and 'liteos_audiod_blob' not in make
assert '$(BUILD)/esp/sbin/audiod: $(USER_AUDIOD_ELF)' in make
assert '$(BUILD)/esp/usr/apps/fat-fsync-probe/fat-fsync-probe: $(DYNAMIC_FSYNC_PROBE_ELF)' in make
assert '$(PHASE13_TEST_IMAGE) $(EXISTING_DIAGNOSTIC_IMAGES)' in make
assert 'EXISTING_DIAGNOSTIC_IMAGES = $(foreach app,' in make
header = (ROOT / 'include/uapi/linux_abi.h').read_text()
assert not re.search(r'\bLITEOS_\w+', header)
source = r'''
#include <uapi/linux_abi.h>
_Static_assert(OS_LINUX_USER_STACK_TOP == 0x00007FFFFFF00000ULL, "stack top");
_Static_assert(OS_LINUX_USER_STACK_SIZE == 8388608ULL, "8 MiB stack");
_Static_assert(OS_LINUX_USER_STACK_BASE == 0x00007FFFFF700000ULL, "stack base");
_Static_assert(OS_LINUX_CLOCK_REALTIME == 0 && OS_LINUX_CLOCK_MONOTONIC == 1, "clock IDs");
'''
with tempfile.TemporaryDirectory(prefix='native-entry-') as directory:
    path = Path(directory)
    lines = make.splitlines()
    index = next(i for i, line in enumerate(lines)
                 if line.startswith('EXISTING_DIAGNOSTIC_IMAGES ='))
    rule = lines[index]
    while rule.rstrip().endswith('\\'):
        index += 1
        rule += '\n' + lines[index]
    fixture = path / 'build'
    sbin = fixture / 'esp/sbin'
    sbin.mkdir(parents=True)
    # A parent make exports command-line assignments through MAKEFLAGS. This
    # isolated fixture must not inspect the real BUILD directory in that case.
    program = f'override BUILD := {fixture}\n{rule}\nall:\n\t@printf "%s\\n" "$(EXISTING_DIAGNOSTIC_IMAGES)"\n'
    for expected in ((), ('fat-fsync-probe',), ('fat-fsync-probe', 'linux-events-test')):
        for name in expected:
            (sbin / name).touch()
        selected = subprocess.check_output(['make', '-s', '-f', '-', 'all'],
                                          input=program, text=True).split()
        assert selected == [str(fixture/'esp/usr/apps'/name/name) for name in expected], selected
    # 旧副本删除后，新归属仍应继续参与后续诊断更新。
    for name in ('fat-fsync-probe', 'linux-events-test'):
        (sbin/name).unlink()
        target=fixture/'esp/usr/apps'/name/name
        target.parent.mkdir(parents=True)
        target.touch()
    selected=subprocess.check_output(['make','-s','-f','-','all'],input=program,text=True).split()
    assert selected==[str(fixture/'esp/usr/apps'/name/name)
                      for name in ('fat-fsync-probe','linux-events-test')],selected
    # 执行实际路径初始化片段，较长的新路径不得覆盖启动路径或参数数组。
    runtime = (ROOT/'kernel/process/runtime_test.c').read_text()
    setup = runtime[runtime.index('    static const char boot_path[]'):runtime.index('    process_t *process = 0;', runtime.index('    static const char boot_path[]'))]
    layout = '#include <stdint.h>\n#include <string.h>\n#include <assert.h>\nint main(void){uint8_t image[0x4000]={0};\n'+setup+r'''
    assert(!strcmp((char*)image+0x3100,"/usr/apps/gshell/gshell"));
    assert(!strcmp((char*)image+0x3180,"/usr/apps/notepad/notepad"));
    assert(!strcmp((char*)image+0x31A0,"/usr/apps/libc-test/libc-test"));
    assert(*(uint64_t*)(void*)(image+0x31D0)==0x004021A0ULL);
    assert(*(uint64_t*)(void*)(image+0x31D8)==0);
    }
'''
    layout_source=path/'layout.c'; layout_source.write_text(layout)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(layout_source),
                    '-o',str(path/'layout')],check=True)
    subprocess.run([str(path/'layout')],check=True)
    blob=(ROOT/'tests/user/user_test_blob.S').read_text()
    assert 'movq $0x004021A0, %rdi' in blob and 'movq $0x004021D0, %rsi' in blob
    check = path / 'abi.c'
    check.write_text(source)
    subprocess.run(['cc', '-std=c11', '-I', str(ROOT / 'include'), '-c', str(check),
                    '-o', str(path / 'abi.o')], check=True)
    obj = path / 'entry.o'
    subprocess.run(['cc', '-m64', '-c', str(ROOT / 'user/libc/crt_start.S'),
                    '-o', str(obj)], check=True)
    code = subprocess.check_output(['objcopy', '-O', 'binary', '-j', '.text',
                                    str(obj), '/dev/stdout'])
    assert len(code) == 38
    assert hashlib.sha256(code).hexdigest() == '229ba0506f65e9a78dd602edfe68b9979c26e87a591eb62472423f4b58dddb88'
    image = path / 'entry.elf'
    # Resolve the external startup routine for this link-only fixture. Never
    # execute it: the real startup path is separately exercised in QEMU.
    subprocess.run(['ld', '--entry=libc_crt_entry', '--defsym=__libc_start_main=0',
                    str(obj), '-o', str(image)], check=True)
    symbols = subprocess.check_output(['nm', str(image)], text=True)
    address = int(re.search(r'^([0-9a-f]+)\s+T\s+libc_crt_entry$', symbols, re.M)[1], 16)
    assert struct.unpack_from('<Q', image.read_bytes(), 24)[0] == address
print('Native startup: unchanged code, ELF entry resolution, stack/clock constants and audio deployment preserved')
