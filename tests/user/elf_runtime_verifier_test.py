"""Real linked ELF fixtures exercise exact runtime checks and false positives."""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('elf_runtime', ROOT / 'tools/verify-elf-runtime.py')
elf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(elf)

with tempfile.TemporaryDirectory(prefix='elf runtime ') as directory:
    base = Path(directory)
    source, obj = base / 'source.c', base / 'source.o'
    source.write_text('void gfx_clear(void) {}\nvoid gfx_fill_rect(void) {}\n'
                      'void gfx_gradient_rect(void) {}\nvoid gfx_frame(void) {}\n')
    subprocess.run(['cc', '-fPIC', '-ffreestanding', '-c', str(source), '-o', str(obj)], check=True)
    loader, library, app = (base / name for name in ('loader', 'library', 'app'))
    for output, soname in ((loader, 'ld-system.so.1'), (library, 'libgraphics.so.1')):
        subprocess.run(['ld', '-shared', '--hash-style=sysv', '-soname', soname,
                        str(obj), '-o', str(output)], check=True)
    source.write_text('extern void gfx_clear(void); void _start(void) {gfx_clear();}\n')
    subprocess.run(['cc', '-fPIC', '-ffreestanding', '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run(['ld', '-pie', '--hash-style=sysv', '-e', '_start',
                    '--dynamic-linker=/lib/ld-system.so.1', str(obj),
                    str(library), '-o', str(app)], check=True)
    env = dict(os.environ, ELFOBJDUMP='objdump')
    command = ['bash', str(ROOT / 'tools/verify-dynamic-loader.sh'),
               str(loader), str(library), str(app)]
    def run(ok):
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
    run(True)
    original = app.read_bytes()
    # 这两个名称都包含旧子串检查所接受的片段。
    for before, after in ((b'/lib/ld-system.so.1', b'/lib/xx-system.so.1'),
                          (b'libgraphics.so.1', b'libgraphics.so.9')):
        assert before in original and len(before) == len(after)
        app.write_bytes(original.replace(before, after))
        run(False)
    app.write_bytes(original)
    original_library = library.read_bytes()
    library.write_bytes(original_library.replace(b'libgraphics.so.1', b'libgraphics.so.9'))
    run(False)
    library.write_bytes(original_library)
    # 节名称和节头不属于运行时身份的一部分。
    stripped = bytearray(original)
    struct.pack_into('<Q', stripped, 40, 0)
    struct.pack_into('<HHH', stripped, 58, 0, 0, 0)
    app.write_bytes(stripped)
    elf.verify(loader, library, app, 'libgraphics.so.1')
    # 直接验证格式错误的 PT_INTERP、程序段和动态字符串边界。
    phoff = struct.unpack_from('<Q', original, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', original, 54)
    programs = [(phoff + i * phsize, struct.unpack_from('<IIQQQQQQ', original,
                  phoff + i * phsize)) for i in range(phnum)]
    position, interp = next(p for p in programs if p[1][0] == 3)
    cases = [original[:63], original[:phoff + phsize - 1]]
    mutation = bytearray(original)
    mutation[interp[2] + interp[5] - 1] = 65
    cases.append(mutation)
    mutation = bytearray(original)
    struct.pack_into('<Q', mutation, position + 32, len(original) + 1)
    cases.append(mutation)
    mutation = bytearray(original)
    dynamic = next(p for _, p in programs if p[0] == 2)
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        if struct.unpack_from('<q', mutation, offset)[0] == 5:
            struct.pack_into('<Q', mutation, offset + 8, 0xffffffffffffffff)
            break
    cases.append(mutation)
    for data in cases:
        app.write_bytes(data)
        try:
            elf.runtime_identity(app)
        except ValueError:
            pass
        else:
            raise AssertionError('malformed ELF accepted')
    app.write_bytes(original)
    # 名称相似的导出符号不能满足必需符号的检查。
    library.write_bytes(original_library.replace(b'gfx_clear\0', b'gfx_cleax\0'))
    run(False)
    library.write_bytes(original_library)
    run(True)
    source.write_text('void gfx_clear_extra(void) {}\nvoid gfx_fill_rect(void) {}\n'
                      'void gfx_gradient_rect(void) {}\nvoid gfx_frame(void) {}\n')
    subprocess.run(['cc', '-fPIC', '-ffreestanding', '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run(['ld', '-shared', '--hash-style=sysv', '-soname', 'libgraphics.so.1',
                    str(obj), '-o', str(library)], check=True)
    # 旧的 grep(gfx_clear) 即使找不到完全匹配的导出项，也会接受 gfx_clear_extra。
    run(False)
    library.write_bytes(original_library)
    fake = base / 'prefix-library'
    source.write_text('void gfx_clear(void) {}\n')
    subprocess.run(['cc', '-fPIC', '-ffreestanding', '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run(['ld', '-shared', '--hash-style=sysv', '-soname', 'libgraphics.so.1-extra',
                    str(obj), '-o', str(fake)], check=True)
    source.write_text('extern void gfx_clear(void); void _start(void) {gfx_clear();}\n')
    subprocess.run(['cc', '-fPIC', '-ffreestanding', '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run(['ld', '-pie', '-e', '_start', '--dynamic-linker=/lib/ld-system.so.1',
                    str(obj), str(fake), '-o', str(app)], check=True)
    run(False)
    app.write_bytes(original)
    run(True)
print('ELF runtime verifier: real links, exact interpreter/dependency/SONAME/exports, sectionless ELF and five malformed inputs passed')
