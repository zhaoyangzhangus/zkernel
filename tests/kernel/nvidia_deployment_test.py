"""Exercise native-driver caching and the real production deployment contract."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('nv_builder', ROOT / 'tools/nvidia/build-driver.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class DriverDeployment(unittest.TestCase):
    def test_cube_build_only_has_no_deployment_side_effects(self):
        import contextlib
        import io
        import os
        from unittest import mock
        stage_spec = importlib.util.spec_from_file_location(
            'nv_stage_build_only', ROOT / 'tools/nvidia/stage-driver.py')
        stage = importlib.util.module_from_spec(stage_spec)
        stage_spec.loader.exec_module(stage)
        previous = Path.cwd()
        with tempfile.TemporaryDirectory() as directory:
            output = io.StringIO()
            try:
                with mock.patch.object(stage, 'ROOT', Path(directory)), \
                     mock.patch.object(sys, 'argv', ['stage-driver.py', '--build-only']), \
                     mock.patch.object(stage, 'build_cube', return_value=Path('build/nvidia/runtime/cube')) as build, \
                     mock.patch.object(stage, 'copy_changed', side_effect=AssertionError('unexpected deployment')), \
                     mock.patch.object(stage.builder, 'FileDigests', side_effect=AssertionError('unexpected payload scan')), \
                     contextlib.redirect_stdout(output):
                    stage.main()
                build.assert_called_once_with(Path('build/nvidia/runtime'),
                                              'x86_64-linux-gnu-gcc',
                                              'build/nvidia/host-deps/glibc')
                self.assertEqual(output.getvalue(), 'CUBE_BUILD_READY=build/nvidia/runtime/cube\n')
                self.assertFalse(Path('build/esp').exists())
                self.assertEqual(list(Path('build/nvidia/runtime').iterdir()), [])
            finally:
                os.chdir(previous)

    def test_cube_builder_keeps_canonical_paths_and_pinned_headers(self):
        from unittest import mock
        stage_spec = importlib.util.spec_from_file_location(
            'nv_stage_cube_builder', ROOT / 'tools/nvidia/stage-driver.py')
        stage = importlib.util.module_from_spec(stage_spec)
        stage_spec.loader.exec_module(stage)
        out = Path('build/nvidia/runtime')
        with mock.patch.object(stage.builder, 'compile_one') as compile_one, \
             mock.patch.object(stage, 'link_cube') as link:
            self.assertEqual(stage.build_cube(out, 'fixture-cc', 'pinned/glibc'), out/'cube')
        (source, obj, compiler, flags), = compile_one.call_args.args
        self.assertEqual((source, obj, compiler),
                         (Path('tools/nvidia/opengl_cube_demo.c'), out/'cube.o', 'fixture-cc'))
        self.assertIn('pinned/glibc/libc6-dev-extract/usr/include', flags)
        self.assertIn('pinned/glibc/libc6-dev-extract/usr/include/x86_64-linux-gnu', flags)
        self.assertIn('-DCUBE_INTERACTIVE=1', flags)
        cube, command, inputs = link.call_args.args
        self.assertEqual(cube, out/'cube')
        self.assertEqual(command[-2:], ['-o', str(out/'cube')+'.tmp'])
        self.assertIn(out/'cube.o', inputs)
        self.assertIn('-nostdlib', command)

    def test_payload_copy_uses_private_temporary_files(self):
        import concurrent.futures
        import threading
        from unittest import mock
        stage_spec = importlib.util.spec_from_file_location(
            'nv_stage_parallel', ROOT / 'tools/nvidia/stage-driver.py')
        stage = importlib.util.module_from_spec(stage_spec)
        stage_spec.loader.exec_module(stage)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source'
            target = Path(directory) / 'payload'
            source.write_bytes(b'verified payload')
            barrier = threading.Barrier(2)
            copy = stage.shutil.copyfile
            def concurrent_copy(source, temporary):
                result = copy(source, temporary)
                barrier.wait(timeout=3)
                return result
            with mock.patch.object(stage.shutil, 'copyfile', side_effect=concurrent_copy):
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                    results = list(pool.map(lambda _: stage.copy_changed(source, target), range(2)))
            self.assertEqual(results, [True, True])
            self.assertEqual(target.read_bytes(), source.read_bytes())
            self.assertEqual(sorted(p.name for p in Path(directory).iterdir()), ['payload', 'source'])

    def test_copied_cache_does_not_validate_against_old_checkout(self):
        import os
        import shutil
        from unittest import mock
        original_cwd = Path.cwd()
        with tempfile.TemporaryDirectory() as directory:
            old = Path(directory) / 'old'
            new = Path(directory) / 'new'
            old.mkdir()
            (old / 'main.c').write_text('#include "value.h"\nint main(void){return VALUE;}\n')
            (old / 'value.h').write_text('#define VALUE 7\n')
            record = (Path('main.c'), Path('build/main.o'), 'gcc', ['-O2'])
            try:
                os.chdir(old)
                with mock.patch.object(builder, 'ROOT', old):
                    self.assertTrue(builder.compile_one(record))
                    self.assertFalse(builder.compile_one(record))
                shutil.copytree(old, new)
                (new / 'value.h').write_text('#define VALUE 19\n')
                os.chdir(new)
                with mock.patch.object(builder, 'ROOT', new):
                    self.assertTrue(builder.compile_one(record))
                    self.assertFalse(builder.compile_one(record))
                subprocess.run(['gcc', 'build/main.o', '-o', 'program'], check=True)
                self.assertEqual(subprocess.run([str(new / 'program')]).returncode, 19)
                self.assertEqual((old / 'value.h').read_text(), '#define VALUE 7\n')
            finally:
                os.chdir(original_cwd)

    def test_tool_version_cache_tracks_replacement_and_path(self):
        import os
        from unittest import mock
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            tools = []
            for name, version in [('first', 'v1'), ('second', 'v2')]:
                parent = base / name
                parent.mkdir()
                tool = parent / 'fixture-compiler'
                tool.write_text('#!/bin/sh\necho ' + version + '\n')
                tool.chmod(0o755)
                tools.append(tool)
            with mock.patch.dict(os.environ, {'PATH': str(tools[0].parent)}):
                self.assertEqual(builder.compiler_identity('fixture-compiler'), 'v1\n')
                before = tools[0].stat()
                tools[0].write_text('#!/bin/sh\necho v3\n')
                os.utime(tools[0], ns=(before.st_atime_ns, before.st_mtime_ns))
                self.assertEqual(builder.compiler_identity('fixture-compiler'), 'v3\n')
            with mock.patch.dict(os.environ, {'PATH': str(tools[1].parent)}):
                self.assertEqual(builder.compiler_identity('fixture-compiler'), 'v2\n')
            self.assertEqual(builder.compiler_identity(str(tools[0])), 'v3\n')
            with self.assertRaises(FileNotFoundError):
                builder.compiler_identity(str(base / 'missing'))

    def test_payload_copy_integrity_and_failure(self):
        from unittest import mock
        stage_spec = importlib.util.spec_from_file_location(
            'nv_stage_copy', ROOT / 'tools/nvidia/stage-driver.py')
        stage = importlib.util.module_from_spec(stage_spec)
        stage_spec.loader.exec_module(stage)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source'
            target = Path(directory) / 'deploy/payload'
            source.write_bytes(b'verified')
            checksum = builder.FileDigests()
            self.assertTrue(stage.copy_changed(source, target, checksum))
            before = target.stat()
            self.assertFalse(stage.copy_changed(source, target, checksum))
            self.assertEqual(target.stat().st_ino, before.st_ino)
            self.assertEqual(target.stat().st_mtime_ns, before.st_mtime_ns)
            target.write_bytes(b'corrupt')
            self.assertTrue(stage.copy_changed(source, target, checksum))
            self.assertEqual(target.read_bytes(), b'verified')
            source.write_bytes(b'next')
            def partial_copy(_source, temporary):
                temporary.write_bytes(b'partial')
                raise OSError('copy failed')
            with mock.patch.object(stage.shutil, 'copyfile', side_effect=partial_copy):
                with self.assertRaisesRegex(OSError, 'copy failed'):
                    stage.copy_changed(source, target, checksum)
            self.assertEqual(target.read_bytes(), b'verified')
            self.assertFalse(target.with_name('payload.nvidia-tmp').exists())
            def changed_source(_source, temporary):
                source.write_bytes(b'concurrent edit')
                temporary.write_bytes(source.read_bytes())
            with mock.patch.object(stage.shutil, 'copyfile', side_effect=changed_source):
                with self.assertRaisesRegex(RuntimeError, 'source changed during copy'):
                    stage.copy_changed(source, target, checksum)
            self.assertEqual(target.read_bytes(), b'verified')
            self.assertFalse(target.with_name('payload.nvidia-tmp').exists())
            self.assertTrue(stage.copy_changed(source, target, checksum))
            self.assertEqual(target.read_bytes(), b'concurrent edit')

    def test_binary_publication_preserves_identical_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'artifact'
            temporary = target.with_suffix('.tmp')
            temporary.write_bytes(b'original')
            expected = builder.publish_binary(temporary, target)
            before = target.stat()
            temporary.write_bytes(b'original')
            self.assertEqual(builder.publish_binary(temporary, target), expected)
            self.assertEqual(target.stat().st_mtime_ns, before.st_mtime_ns)
            self.assertEqual(target.stat().st_ino, before.st_ino)
            self.assertFalse(temporary.exists())
            temporary.write_bytes(b'new')
            self.assertNotEqual(builder.publish_binary(temporary, target), expected)
            self.assertEqual(target.read_bytes(), b'new')
            with self.assertRaises(FileNotFoundError):
                builder.publish_binary(temporary, target)
            self.assertEqual(target.read_bytes(), b'new')

    def test_cube_link_output_integrity_and_failure(self):
        from unittest import mock
        stage_spec = importlib.util.spec_from_file_location(
            'nv_stage', ROOT / 'tools/nvidia/stage-driver.py')
        stage = importlib.util.module_from_spec(stage_spec)
        stage_spec.loader.exec_module(stage)
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source, obj, cube = base / 'main.c', base / 'main.o', base / 'cube'
            source.write_text('int main(void) { return 7; }\n')
            builder.compile_one((source, obj, 'gcc', ['-O2']))
            command = ['gcc', '-Wl,--build-id=none', str(obj), '-o', str(cube) + '.tmp']
            self.assertTrue(stage.link_cube(cube, command, [obj]))
            expected = builder.sha(cube)
            before = cube.stat()
            self.assertFalse(stage.link_cube(cube, command, [obj]))
            self.assertEqual(subprocess.run([str(cube)]).returncode, 7)
            # Missing/legacy/corrupt receipts revalidate without changing bytes.
            receipt = cube.with_suffix('.link.json')
            for data in ('broken', '[]', '{"command":[]}'):
                receipt.write_text(data)
                self.assertTrue(stage.link_cube(cube, command, [obj]))
                self.assertEqual(builder.sha(cube), expected)
                self.assertEqual(cube.stat().st_mtime_ns, before.st_mtime_ns)
                self.assertEqual(cube.stat().st_ino, before.st_ino)
            cube.write_bytes(b'corrupt ELF')
            self.assertTrue(stage.link_cube(cube, command, [obj]))
            self.assertEqual(builder.sha(cube), expected)
            receipt_before = receipt.read_bytes()
            changed_command = command + ['-invalid-link-flag']
            with mock.patch.object(stage.subprocess, 'run',
                                   side_effect=subprocess.CalledProcessError(1, changed_command)):
                with self.assertRaises(subprocess.CalledProcessError):
                    stage.link_cube(cube, changed_command, [obj])
            self.assertEqual(builder.sha(cube), expected)
            self.assertEqual(receipt.read_bytes(), receipt_before)
            source.write_text('int main(void) { return 19; }\n')
            builder.compile_one((source, obj, 'gcc', ['-O2']))
            self.assertTrue(stage.link_cube(cube, command, [obj]))
            self.assertNotEqual(builder.sha(cube), expected)
            self.assertEqual(subprocess.run([str(cube)]).returncode, 19)

    def test_frozen_input_snapshot_rejects_mid_build_edit(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / 'header.h'
            file.write_text('first')
            digests = builder.FileDigests(snapshot=True)
            before = digests(file)
            self.assertEqual(digests(file), before)
            digests.validate()
            file.write_text('second')
            self.assertEqual(digests(file), before)
            with self.assertRaisesRegex(RuntimeError, 'input changed during build'):
                digests.validate()
            refreshed = builder.FileDigests(snapshot=True)
            self.assertEqual(refreshed(file), builder.sha(file))
            refreshed.validate()

    def test_shared_checksums_detect_replacement_and_restored_mtime(self):
        import concurrent.futures
        import os
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / 'header.h'
            file.write_text('aaaa')
            digests = builder.FileDigests()
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                results = list(pool.map(digests, [file] * 64))
            self.assertEqual(len(set(results)), 1)
            self.assertEqual(digests.reads, 1)
            self.assertEqual(digests.hits, 63)
            before = file.stat()
            file.write_text('bbbb')
            os.utime(file, ns=(before.st_atime_ns, before.st_mtime_ns))
            self.assertNotEqual(digests(file), results[0])
            self.assertEqual(digests(file), builder.sha(file))
            replacement = file.with_suffix('.tmp')
            replacement.write_text('cccc')
            os.utime(replacement, ns=(before.st_atime_ns, before.st_mtime_ns))
            os.replace(replacement, file)
            self.assertEqual(digests(file), builder.sha(file))
            self.assertEqual(digests.reads, 3)

    def test_weakened_object_corruption_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source, obj = base / 'stub.c', base / 'stub.o'
            source.write_text('int fallback(void) { return 0; }\n')
            builder.compile_one((source, obj, 'gcc', ['-O2']))
            digests = builder.FileDigests()
            weak = builder.weaken_object(obj, 'objcopy', digests)
            stamp = weak.stat().st_mtime_ns
            expected = builder.sha(weak)
            builder.weaken_object(obj, 'objcopy', digests)
            self.assertEqual(weak.stat().st_mtime_ns, stamp)
            weak.write_bytes(b'corrupt')
            builder.weaken_object(obj, 'objcopy', digests)
            self.assertEqual(builder.sha(weak), expected)
            weak.with_suffix('.json').write_text('[]')
            builder.weaken_object(obj, 'objcopy', digests)
            self.assertEqual(builder.sha(weak), expected)
            source.write_text('int fallback(void) { return 17; }\n')
            builder.compile_one((source, obj, 'gcc', ['-O2']), digests)
            builder.weaken_object(obj, 'objcopy', digests)
            self.assertNotEqual(builder.sha(weak), expected)
            self.assertRegex(subprocess.check_output(['nm', str(weak)], text=True),
                             r'(?m)^[0-9a-f]+ W fallback$')

    def test_invalid_receipt_types_are_cache_misses(self):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / 'receipt.json'
            for text in ('broken', '[]', 'null', 'true', '42', '"text"'):
                file.write_text(text)
                self.assertEqual(builder.read_receipt(file), {})
            file.write_text('{"ok":1}')
            self.assertEqual(builder.read_receipt(file), {'ok': 1})
            self.assertEqual(builder.read_receipt(file.with_suffix('.missing')), {})

    def test_cube_fps_not_clamped(self):
        source = (ROOT / 'tools/nvidia/opengl_cube_demo.c').read_text()
        functions = 'static char *cube_overlay_u64(' + source.split(
            'static char *cube_overlay_u64(', 1)[1].split(
            'static void cube_draw_overlay(', 1)[0]
        harness = '''
#include <stdint.h>
#include <string.h>
#include <assert.h>
''' + functions + '''
int main(void) {
 char text[64];
 cube_format_overlay(text, 12345, 1000000000ULL);
 assert(!strcmp(text, "FPS:12345 TIME:1.0S"));
 cube_format_overlay(text, 0, 0);
 assert(!strcmp(text, "FPS:0 TIME:0.0S"));
 cube_format_overlay(text, UINT32_MAX, 1);
 assert(!strcmp(text, "FPS:4294967295000000000 TIME:0.0S"));
 cube_format_overlay(text, 1000, 1234500000000ULL);
 assert(!strcmp(text, "FPS:0 TIME:1234.5S"));
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / 'overlay.c'
            binary = Path(directory) / 'overlay'
            fixture.write_text(harness)
            subprocess.run(['gcc', '-O2', '-Wall', '-Werror', str(fixture),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_adapter_irq_scope(self):
        source = (ROOT / 'kernel/drivers/gpu/nvidia/rm/nv_rm_vendor_bridge.c').read_text()
        function = source.split('static bool nv_rm_vendor_init_adapter(', 1)[1].split('\nkstatus_t nv_rm_vendor_start', 1)[0]
        function = 'static bool nv_rm_vendor_init_adapter(' + function
        function = function.replace('__asm__ volatile ("pushfq; popq %0" : "=r"(cpu_flags) : : "memory");', 'cpu_flags = irq_enabled ? (1ULL << 9) : 0;')
        function = function.replace('__asm__ volatile ("sti" : : : "memory");', 'irq_enabled = 1;')
        function = function.replace('__asm__ volatile ("cli" : : : "memory");', 'irq_enabled = 0;')
        harness = '''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define NV_FALSE 0
typedef struct {
 void *stack; void *state;
 uint32_t irq_top_hits, irq_rm_handled_hits, irq_bottom_hits;
 uint32_t irq_bottom_pending, irq_bottom_schedule_failed;
} nv_rm_vendor_context_t;
#define atomic_load(pointer) (*(pointer))
static volatile uint32_t g_nv_rm_boot_diagnostic[8];
static uint32_t x86_current_apic_id(void) { return 1U; }
static int irq_enabled, preempt_depth, result;
static bool sched_preempt_disabled(void) { return preempt_depth != 0; }
static int rm_init_adapter(void *stack, void *state) {
 (void)stack; (void)state; assert(irq_enabled); assert(preempt_depth == 0);
 irq_enabled=0; return result;
}
''' + function + '''
int main(void) {
 nv_rm_vendor_context_t context={0};
 for (int initial=0;initial<2;initial++) for (int success=0;success<2;success++) {
  irq_enabled=initial; result=success;
  assert(nv_rm_vendor_init_adapter(&context)==(bool)success);
  assert(irq_enabled==initial); assert(!preempt_depth);
  assert(g_nv_rm_boot_diagnostic[0]==(success ? 2U : 3U));
  assert(g_nv_rm_boot_diagnostic[1]==0U && g_nv_rm_boot_diagnostic[2]==1U);
 }
 preempt_depth=1; irq_enabled=0;
 assert(!nv_rm_vendor_init_adapter(&context));
 assert(!irq_enabled && preempt_depth==1);
 assert(g_nv_rm_boot_diagnostic[0]==1U && g_nv_rm_boot_diagnostic[1]==1U);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            fixture = base / 'scope.c'
            binary = base / 'scope'
            fixture.write_text(harness)
            subprocess.run(['gcc', '-O2', '-Wall', '-Werror', str(fixture), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
            # Negative control: omitting interrupt enable must fail in RM.
            fixture.write_text(harness.replace('irq_enabled = 1;', '(void)0;', 1))
            subprocess.run(['gcc', '-O2', str(fixture), '-o', str(binary)], check=True)
            self.assertNotEqual(subprocess.run([str(binary)], stderr=subprocess.DEVNULL).returncode, 0)

    def test_incremental_dependency_and_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            header = base / 'value.h'
            header.write_text('#define VALUE 1\n')
            source = base / 'test.c'
            source.write_text('#include "value.h"\nint value(void) { return VALUE; }\n')
            output = base / 'test.o'
            record = (source, output, 'gcc', ['-O0'])
            self.assertTrue(builder.compile_one(record))
            self.assertFalse(builder.compile_one(record))
            before = builder.sha(output)
            header.write_text('#define VALUE 2\n')
            self.assertTrue(builder.compile_one(record))
            self.assertNotEqual(before, builder.sha(output))
            output.write_bytes(b'corrupt')
            self.assertTrue(builder.compile_one(record))
            output.with_suffix('.json').write_text('broken JSON')
            self.assertTrue(builder.compile_one(record))

class PayloadDeployment(unittest.TestCase):
    def test_current_kernel_has_real_driver(self):
        kernel = ROOT / 'build/esp/EFI/OS/kernel.elf'
        symbols = subprocess.check_output(['nm', str(kernel)], text=True)
        for name in ('nv_rm_vendor_start', 'nv_get_firmware', 'nv_alloc_kernel_mapping',
                     'os_queue_work_item'):
            self.assertRegex(symbols, r'(?m)^[0-9a-f]+ T ' + name + r'$')
        undefined = subprocess.check_output(['nm', '-u', str(kernel)], text=True)
        self.assertFalse(undefined.strip())

    def test_payload_inventory(self):
        import json
        esp = ROOT / 'build/esp'
        inventory = json.loads((ROOT / 'build/nvidia/runtime/deployed.json').read_text())
        for name, digest in inventory.items():
            self.assertEqual(builder.sha(esp / name), digest, name)
        for name in ('usr/apps/cube/cube', 'EFI/OS/firmware/gsp_ga10x.bin',
                     'usr/apps/cube/lib/libc.so.6',
                     'usr/apps/cube/lib/libEGL_nvidia.so.0'):
            self.assertIn(name, inventory)
        interpreter = subprocess.check_output(['readelf', '-l', str(esp / 'usr/apps/cube/cube')], text=True)
        self.assertIn('/usr/apps/cube/lib/ld-linux-x86-64.so.2', interpreter)

    def test_elf_blob_sections(self):
        for name in ('user-init-blob', 'user-test-blob', 'trampoline', 'uaccess-entry', 'interrupt'):
            path = ROOT / 'build/kernel' / (name + '.o')
            sections = subprocess.check_output(['objdump', '-h', str(path)], text=True)
            section = sections.split('.rdata', 1)[1].split('\n', 2)[1]
            self.assertIn('ALLOC', section)
            self.assertIn('LOAD', section)
        self.assertGreater((ROOT / 'build/kernel/user-init-blob.bin').stat().st_size, 0)

    def test_production_failure_does_not_reboot(self):
        source = (ROOT / 'kernel/init/runtime.c').read_text()
        block = source.split('#if OS_NVIDIA_PHASE5\n    if (userspace_device_init_done()) {', 1)[1].split('#elif', 1)[0]
        self.assertIn('if (realtest_enabled())\n                realtest_finish_failure();', block)
        self.assertIn('} else {\n            kernel_serial_write_serial_only("OS_NVIDIA_GSP_READY', block)
        config = (ROOT / 'build/kernel/.debug-serial').read_text().strip().split(':')
        self.assertEqual(config[0], 'os-config-v1')
        self.assertEqual(len(config), 12)
        self.assertTrue(all(value in ('0', '1') for value in config[1:]))
        self.assertEqual(config[-1], '1')


if __name__ == '__main__':
    if '--host-only' in sys.argv:
        sys.argv.remove('--host-only')
        unittest.main(defaultTest='DriverDeployment')
    else:
        unittest.main()
