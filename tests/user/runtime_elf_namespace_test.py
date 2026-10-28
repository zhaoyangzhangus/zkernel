"""The neutral runtime names must preserve the standard ELF64 wire format."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
header = (ROOT / 'user/runtime/elf64.h').read_text()
loader = (ROOT / 'user/runtime/ld.c').read_text()
assert not re.search(r'\b(?:liteos_rt_|LITEOS_RT_)', header + loader)
assert 'uint64_t runtime_linker_main(uint64_t initial_stack)' in loader
assert 'call runtime_linker_main' in (ROOT / 'user/runtime/ld_start.S').read_text()
make = (ROOT / 'makefile').read_text()
assert '@ELFOBJDUMP="$(ELFOBJDUMP)" bash tools/verify-dynamic-loader.sh' in make
assert 'DYNAMIC_LOADER_ELF = $(ELF_RUNTIME_DIR)/ld-system.so.1' in make
assert '--soname ld-system.so.1' in make
assert not re.search(r'-dynamic-linker[^\n]*liteos', make)
assert '$(BUILD)/esp/lib/ld-linux-system.so.1:' in make
assert '$(BUILD)/esp/lib/ld-liteos.so.1' not in make
assert '$(BUILD)/esp/lib/ld-linux-liteos.so.1' not in make
assert 'DYNAMIC_LIBC_ELF = $(ELF_RUNTIME_DIR)/libsystemc.so.1' in make
assert '--soname libsystemc.so.1' in make
assert '$(BUILD)/esp/lib/libliteosc.so.1' not in make
# Already-deployed libc interprets this TLS header: changing its numeric magic
# is an ABI change, not a naming cleanup.
assert re.search(r'#define\s+OS_RT_TLS_MAGIC\s+0x4C4954454F53544CULL', loader)
fields = (
    ('header', 'Elf64_Ehdr', 'ident:e_ident type:e_type machine:e_machine version:e_version entry:e_entry program_header_offset:e_phoff section_header_offset:e_shoff flags:e_flags header_size:e_ehsize program_header_size:e_phentsize program_header_count:e_phnum section_header_size:e_shentsize section_header_count:e_shnum section_name_index:e_shstrndx'),
    ('program_header', 'Elf64_Phdr', 'type:p_type flags:p_flags offset:p_offset virtual_address:p_vaddr physical_address:p_paddr file_size:p_filesz memory_size:p_memsz alignment:p_align'),
    ('dynamic', 'Elf64_Dyn', 'tag:d_tag value:d_un'),
    ('symbol', 'Elf64_Sym', 'name:st_name info:st_info other:st_other section:st_shndx value:st_value size:st_size'),
    ('rela', 'Elf64_Rela', 'offset:r_offset info:r_info addend:r_addend'),
)
code = '#include <elf.h>\n#include <stddef.h>\n#include <assert.h>\n#include "user/runtime/elf64.h"\n'
for name, standard, members in fields:
    own = f'runtime_elf64_{name}_t'
    code += f'_Static_assert(sizeof({own}) == sizeof({standard}), "{name} size");\n'
    for member in members.split():
        left, right = member.split(':')
        code += f'_Static_assert(offsetof({own},{left}) == offsetof({standard},{right}), "{name}.{left}");\n'
special = {'ELF_CLASS_64': 'ELFCLASS64', 'ELF_DATA_LSB': 'ELFDATA2LSB',
           'ELF_VERSION': 'EV_CURRENT', 'ELF_MACHINE_X86_64': 'EM_X86_64',
           'DT_RELA_TYPE': 'DT_RELA'}
constants = re.findall(r'^#define\s+OS_RT_(\w+)', header, re.M)
for name in constants:
    standard = special.get(name, name)
    code += f'_Static_assert(OS_RT_{name} == {standard}, "{name}");\n'
code += '''int main(void) {
    assert(runtime_elf64_symbol_bind(0xa7) == ELF64_ST_BIND(0xa7));
    uint64_t info = ELF64_R_INFO(0x12345678U, R_X86_64_JUMP_SLOT);
    assert(runtime_elf64_relocation_symbol(info) == ELF64_R_SYM(info));
    assert(runtime_elf64_relocation_type(info) == ELF64_R_TYPE(info));
}
'''
font = (ROOT / 'user/runtime/blend2d_demo.h').read_text()
for name, value in (('MIN', 14), ('DEFAULT', 22), ('MAX', 50), ('STEP', 4)):
    assert re.search(r'OS_BLEND2D_FONT_' + name + r'\s*=\s*' + str(value) + 'U', font)
with tempfile.TemporaryDirectory(prefix='runtime-elf-') as directory:
    source = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    source.write_text(code)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    # Compile actual loader dependency traversal with only file I/O replaced.
    # This covers alias normalization, recursive cycles and capacity checks.
    def function(signature):
        start = loader.index(signature)
        end = loader.index('\n}', start) + 2
        return loader[start:end] + '\n'
    aliases = '#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n#include <assert.h>\n#include <string.h>\n'
    aliases += '#include "user/runtime/elf64.h"\n'
    aliases += '\n'.join(re.findall(r'^#define OS_RT_.*', loader, re.M)) + '\n'
    start = loader.index('typedef struct loaded_image {')
    end = loader.index('} loaded_image_t;', start) + len('} loaded_image_t;')
    aliases += loader[start:end] + '\n'
    for signature in ('static bool copy_string(', 'static bool same_string(',
                      'static bool bounded_string(', 'static bool make_library_path(',
                      'static const char *canonical_library_name(', 'static int32_t find_image('):
        aliases += function(signature)
    aliases += r'''
static unsigned loads;
static bool fail_load;
static bool load_library_file(const char *path, const char *name,
                              uint64_t *next_base, loaded_image_t *image) {
    ++loads;
    assert(strcmp(path, "/lib/libsystemc.so.1") == 0);
    assert(strcmp(name, "libsystemc.so.1") == 0);
    if (fail_load) return false;
    image->used = true;
    assert(copy_string(image->name, sizeof(image->name), name));
    ++*next_base;
    static const char cycle[] = "libsystemc.so.1";
    image->string_table = (uintptr_t)cycle;
    image->string_size = sizeof(cycle);
    image->needed_count = 1;
    return true;
}
'''
    aliases += function('static bool load_needed(')
    aliases += r'''
int main(void) {
    const char *names[] = {"libsystemc.so.1", "/lib/libsystemc.so.1"};
    for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 2; ++j) {
        loaded_image_t images[OS_RT_MAX_IMAGES] = {0};
        char strings[128];
        strcpy(strings, names[i]);
        size_t second = strlen(names[i]) + 1;
        strcpy(strings + second, names[j]);
        images[0].string_table = (uintptr_t)strings;
        images[0].string_size = second + strlen(names[j]) + 1;
        images[0].needed_count = 2;
        images[0].needed[1] = second;
        uint32_t count = 1;
        uint64_t base = 0;
        loads = 0;
        assert(load_needed(images, &count, &base, 0));
        assert(count == 2 && loads == 1 && base == 1);
        assert(strcmp(images[1].name, "libsystemc.so.1") == 0);
        // A cached alias remains usable even with the image array full.
        count = OS_RT_MAX_IMAGES;
        assert(load_needed(images, &count, &base, 0));
        assert(loads == 1 && base == 1);
        images[1].used = false;
        assert(!load_needed(images, &count, &base, 0));
        assert(loads == 1);
        count = 1; fail_load = true;
        assert(!load_needed(images, &count, &base, 0));
        assert(count == 1 && base == 1 && loads == 2);
        fail_load = false;
        images[0].needed[0] = images[0].string_size;
        assert(!load_needed(images, &count, &base, 0));
        images[0].needed[0] = 0;
        images[0].string_size = strlen(names[i]); // no terminator in bounds
        assert(!load_needed(images, &count, &base, 0));
    }
    const char *other[] = {NULL, "", "libgraphics.so.1", "libliteosc.so.1", "/lib/libliteosc.so.1", "libliteosc.so.1-extra",
                           "/other/libliteosc.so.1", "LIBLITEOSC.SO.1"};
    for (unsigned i = 0; i < sizeof(other)/sizeof(other[0]); ++i)
        assert(canonical_library_name(other[i]) == other[i]);
}
'''
    source.write_text(aliases)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(ROOT), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    # Execute the real kernel whitelist, not a reimplementation of its rules.
    linux = (ROOT / 'kernel/syscall/linux.c').read_text()
    start = linux.index('static bool linux_string_equal(')
    end = linux.index('\nstatic int64_t linux_arch_prctl(', start)
    selection = '#include <stdbool.h>\n#include <stddef.h>\n#include <assert.h>\n' + linux[start:end]
    selection += r'''
int main(void) {
    const char *linux[] = {"/lib/ld-linux-system.so.1",
        "/usr/apps/chrome/lib/ld-linux-x86-64.so.2", "/usr/apps/cube/lib/ld-linux-x86-64.so.2",
        "/usr/apps/vulkan-probe/lib/ld-linux-x86-64.so.2"};
    for (unsigned i = 0; i < sizeof(linux)/sizeof(linux[0]); ++i)
        assert(linux_elf_interpreter_path(linux[i]));
    const char *native[] = {"/lib64/ld-linux-x86-64.so.2", "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
        "/usr/apps//lib/ld-linux-x86-64.so.2", "/usr/apps/../lib/ld-linux-x86-64.so.2",
        "/usr/apps/chrome/../lib/ld-linux-x86-64.so.2", "/usr/apps/chrome/lib/ld-linux-x86-64.so.2-extra", NULL, "", "/lib/ld-system.so.1", "/lib/ld-liteos.so.1", "/lib/ld-linux-liteos.so.1",
        "/lib/ld-linux-system.so.1-extra", "/lib/ld-linux-system.so.",
        "/lib/ld-linux-system.so.1/", "/LIB/ld-linux-system.so.1"};
    for (unsigned i = 0; i < sizeof(native)/sizeof(native[0]); ++i)
        assert(!linux_elf_interpreter_path(native[i]));
}
'''
    source.write_text(selection)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print(f'Runtime ELF: five standard structures, all field offsets, {len(constants)} constants and relocation helpers passed')
print('Interpreter namespace: neutral deployment, real kernel native/Linux separation and retired interpreter rejection passed')
print('Library namespace: actual traversal, four absolute/SONAME graphs, cycles, full cache, failed I/O and malformed names passed')
