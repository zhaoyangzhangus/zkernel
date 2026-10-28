"""Real ELF staging, cache integrity and interrupted publication regressions."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('chrome_stage', ROOT / 'tools/chromium/stage-binary.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class ChromeStaging(unittest.TestCase):
    def test_fixed_mount_permissions_reuse_cache_without_skipping_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            code, source, target = base/'main.c', base/'source', base/'chrome'
            code.write_text('int main(void) { return 0; }\n')
            subprocess.run(['gcc', '-g', str(code), '-o', str(source)], check=True)
            strip = Path(shutil.which('strip'))
            original_chmod = Path.chmod
            def fixed_mode(path, mode, **kwargs):
                return original_chmod(path, 0o777, **kwargs)
            with mock.patch.object(Path, 'chmod', fixed_mode):
                self.assertTrue(stage.stage_binary(source, target, strip))
                before = target.stat()
                self.assertEqual(before.st_mode & 0o7777, 0o777)
                with mock.patch.object(stage.subprocess, 'run') as run:
                    self.assertFalse(stage.stage_binary(source, target, strip))
                    run.assert_not_called()
                self.assertEqual(target.stat().st_ino, before.st_ino)
                self.assertEqual(target.stat().st_mtime_ns, before.st_mtime_ns)
                target.write_bytes(b'corrupt')
                self.assertTrue(stage.stage_binary(source, target, strip))
                stage.validate_executable(target)
            self.assertFalse(list(base.glob('.chrome-mode-*')))

    def test_real_shell_pipeline_is_incremental_and_lock_safe(self):
        import fcntl
        with tempfile.TemporaryDirectory(prefix='chrome stage ') as directory:
            base = Path(directory)
            out = base / 'chromium/out/Default'
            build = base / 'build'
            esp = build / 'esp'
            bundle = esp / 'usr/apps/chrome'
            out.mkdir(parents=True)
            (out/'args.gn').write_text('native_os_runtime = "system"\nenable_vulkan = false\nproprietary_codecs = false\n')
            (build/'elf').mkdir(parents=True)
            (build/'elf/chrome-start.elf').write_bytes(b'launcher fixture')
            (esp / 'lib').mkdir(parents=True)
            for name in ['ld-system.so.1', 'ld-linux-system.so.1', 'ld-linux-liteos.so.1',
                         'libsystemc.so.1', 'libliteosc.so.1']:
                (esp / 'lib' / name).write_bytes(b'fixture')
            (out / 'main.c').write_text('int main(void) { return 7; }\n')
            (out / 'runtime.c').write_text('int runtime_fixture(void) { return 1; }\n')
            subprocess.run(['gcc', '-shared', '-fPIC', '-nostdlib',
                            '-Wl,-soname,libsystemc.so.1', str(out/'runtime.c'),
                            '-o', str(out/'runtime.so')], check=True)
            subprocess.run(['gcc', '-g', str(out / 'main.c'), '-o', str(out / 'chrome')], check=True)
            (out / 'build.ninja').write_text(
                'rule compile\n  command = gcc -g -nostdlib -Wl,-e,main main.c -Wl,--no-as-needed runtime.so '
                '-Wl,--dynamic-linker,/lib/ld-linux-system.so.1 -o chrome\n'
                'build chrome: compile main.c runtime.so\n')
            subprocess.run(['ninja', '-C', str(out), '-j1', 'chrome'],
                           check=True, capture_output=True)
            (out / 'resources.pak').write_bytes(b'resources')
            (out / 'icudtl.dat').write_bytes(b'data')
            (out / 'chrome_sandbox').write_bytes(b'sandbox')
            for name in ['locales/en-US.pak', 'resources/nested/.hidden', 'MEIPreload/manifest.json']:
                path = out / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(name.encode())
            (out / 'resources/alias').symlink_to('nested/.hidden')
            font, license = base / 'font', base / 'license'
            font.write_bytes(b'font'); license.write_bytes(b'license')
            shim = base / 'host-state-fixtures'
            shim.mkdir()
            # Isolate host job-state queries, not the real Ninja/ELF/copy operations.
            for name in ['pgrep', 'systemctl']:
                path = shim / name
                path.write_text('#!/bin/sh\nexit 1\n')
                path.chmod(0o755)
            # Launcher graph status has its own failure/success gate tests.
            # Keep this resource/ELF publication fixture independent of the
            # project's current native launcher build configuration.
            (shim/'make').write_text('#!/bin/sh\nexit 0\n')
            (shim/'make').chmod(0o755)
            env = dict(os.environ, PATH=str(shim) + os.pathsep + os.environ['PATH'],
                       LLVM_STRIP=shutil.which('strip'), OS_CJK_FONT=str(font),
                       OS_CJK_FONT_LICENSE=str(license))
            command = ['bash', str(ROOT / 'tools/stage-chrome.sh'),
                       '--chromium-out', str(out), '--build', str(build)]
            def run():
                return subprocess.run(command, env=env, capture_output=True, text=True)
            original_files = sorted(str(p.relative_to(build)) for p in build.rglob('*'))
            strip = env['LLVM_STRIP']
            env['LLVM_STRIP'] = str(base / 'missing-strip')
            missing_tool = run()
            self.assertNotEqual(missing_tool.returncode, 0)
            self.assertIn('找不到 llvm-strip', missing_tool.stderr)
            self.assertEqual(sorted(str(p.relative_to(build)) for p in build.rglob('*')), original_files)
            env['LLVM_STRIP'] = strip
            first = run()
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertIn('CHROME_BINARY_STAGE=updated', first.stdout)
            targets = [bundle / name for name in
                       ['chrome', 'resources.pak', 'icudtl.dat', 'chrome_sandbox']]
            targets += [esp / 'etc/fonts/NotoSansCJK-Regular.ttc',
                        esp / 'etc/fonts/NotoSansCJK-LICENSE', build / 'chrome-staging.manifest']
            targets += [bundle / name for name in
                        ['locales/en-US.pak', 'resources/nested/.hidden', 'MEIPreload/manifest.json']]
            self.assertFalse((esp / 'sbin/chrome').exists())
            self.assertEqual((bundle / 'chrome-start').read_bytes(), b'launcher fixture')
            alias = bundle / 'resources/alias'
            alias_before = (alias.lstat().st_ino, alias.lstat().st_mtime_ns, os.readlink(alias))
            retained = bundle / 'resources/retained'
            retained.write_bytes(b'keep destination-only files')
            # Include the destination-only file in the initial manifest.
            self.assertEqual(run().returncode, 0)
            before = {p: (p.stat().st_ino, p.stat().st_mtime_ns, stage.sha(p)) for p in targets}
            second = run()
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertIn('CHROME_BINARY_STAGE=cached', second.stdout)
            for path, identity in before.items():
                self.assertEqual((path.stat().st_ino, path.stat().st_mtime_ns, stage.sha(path)), identity)
            self.assertEqual((alias.lstat().st_ino, alias.lstat().st_mtime_ns, os.readlink(alias)), alias_before)
            self.assertEqual(retained.read_bytes(), b'keep destination-only files')
            nested = out / 'resources/nested/.hidden'
            nested.write_bytes(b'changed nested resource')
            self.assertEqual(run().returncode, 0)
            self.assertEqual((bundle / 'resources/nested/.hidden').read_bytes(), nested.read_bytes())
            nested.chmod(0o600)
            self.assertEqual(run().returncode, 0)
            self.assertEqual((bundle / 'resources/nested/.hidden').stat().st_mode & 0o7777, 0o600)
            (out / 'resources/alias').unlink()
            (out / 'resources/alias').symlink_to('../locales/en-US.pak')
            self.assertEqual(run().returncode, 0)
            self.assertEqual(os.readlink(alias), '../locales/en-US.pak')
            nested_target = bundle / 'resources/nested/.hidden'
            nested_before = (nested_target.stat().st_ino, nested_target.stat().st_mtime_ns,
                             nested_target.read_bytes())
            manifest = build / 'chrome-staging.manifest'
            manifest_before = manifest.read_bytes()
            nested.write_bytes(b'next nested resource')
            copy = shim / 'cp'
            copy.write_text('#!/bin/bash\nprintf partial > "${@: -1}"\nexit 1\n')
            copy.chmod(0o755)
            self.assertNotEqual(run().returncode, 0)
            self.assertEqual((nested_target.stat().st_ino, nested_target.stat().st_mtime_ns,
                              nested_target.read_bytes()), nested_before)
            self.assertEqual(manifest.read_bytes(), manifest_before)
            self.assertFalse(list(esp.rglob('.chrome-resource.*')))
            copy.unlink()
            self.assertEqual(run().returncode, 0)
            (out / 'resources.pak').write_bytes(b'new resources')
            self.assertEqual(run().returncode, 0)
            resource = bundle / 'resources.pak'
            self.assertEqual(resource.read_bytes(), b'new resources')
            resource.chmod(0o600)
            self.assertEqual(run().returncode, 0)
            self.assertEqual(resource.stat().st_mode & 0o7777, 0o644)
            manifest = build / 'chrome-staging.manifest'
            manifest_before = (manifest.stat().st_ino, manifest.stat().st_mtime_ns, manifest.read_bytes())
            checksum = shim / 'sha256sum'
            checksum.write_text('#!/bin/sh\necho incomplete-manifest\nexit 1\n')
            checksum.chmod(0o755)
            self.assertNotEqual(run().returncode, 0)
            self.assertEqual((manifest.stat().st_ino, manifest.stat().st_mtime_ns,
                              manifest.read_bytes()), manifest_before)
            self.assertFalse(list(build.glob('.chrome-manifest.*')))
            checksum.unlink()
            with (build / '.chromium-build.lock').open('w') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                blocked = run()
                self.assertNotEqual(blocked.returncode, 0)
                self.assertIn('构建仍在运行', blocked.stderr)
            (out / 'resources.pak').write_bytes(b'next resources')
            install = shim / 'install'
            install.write_text('#!/bin/bash\nprintf partial > "${@: -1}"\nexit 1\n')
            install.chmod(0o755)
            failed = run()
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(resource.read_bytes(), b'new resources')
            self.assertFalse(list(esp.rglob('*.stage.*')))

    def test_strip_tool_change_invalidates_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source, target, tool = base / 'source', base / 'chrome', base / 'strip'
            code = base / 'main.c'
            code.write_text('int main(void) { return 0; }\n')
            subprocess.run(['gcc', '-g', str(code), '-o', str(source)], check=True)
            shutil.copyfile(shutil.which('strip'), tool)
            tool.chmod(0o755)
            self.assertTrue(stage.stage_binary(source, target, tool))
            self.assertFalse(stage.stage_binary(source, target, tool))
            before = target.stat()
            with tool.open('ab') as stream:
                stream.write(b'new tool generation')
            self.assertTrue(stage.stage_binary(source, target, tool))
            self.assertEqual(target.stat().st_ino, before.st_ino)
            self.assertEqual(target.stat().st_mtime_ns, before.st_mtime_ns)

    def test_real_elf_cache_corruption_and_receipts(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source, target = base / 'chrome', base / 'esp/chrome'
            code = base / 'main.c'
            code.write_text('int main(void) { return 7; }\n')
            subprocess.run(['gcc', '-g', str(code), '-o', str(source)], check=True)
            strip = Path(shutil.which('strip'))
            self.assertTrue(stage.stage_binary(source, target, strip))
            expected, before = stage.sha(target), target.stat()
            self.assertLess(target.stat().st_size, source.stat().st_size)
            self.assertEqual(subprocess.run([str(target)]).returncode, 7)
            with mock.patch.object(stage.subprocess, 'run') as run:
                self.assertFalse(stage.stage_binary(source, target, strip))
                run.assert_not_called()
            for invalid in ['[]', 'broken', 'null']:
                target.with_name('chrome.staging.json').write_text(invalid)
                self.assertTrue(stage.stage_binary(source, target, strip))
                self.assertEqual(target.stat().st_ino, before.st_ino)
                self.assertEqual(target.stat().st_mtime_ns, before.st_mtime_ns)
            target.write_bytes(b'corrupt')
            self.assertTrue(stage.stage_binary(source, target, strip))
            self.assertEqual(stage.sha(target), expected)
            target.chmod(0o644)
            self.assertTrue(stage.stage_binary(source, target, strip))
            self.assertEqual(target.stat().st_mode & 0o7777, 0o755)
            code.write_text('int main(void) { return 19; }\n')
            subprocess.run(['gcc', '-g', str(code), '-o', str(source)], check=True)
            self.assertTrue(stage.stage_binary(source, target, strip))
            self.assertEqual(subprocess.run([str(target)]).returncode, 19)

    def test_strip_failure_and_source_edit_preserve_old_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source, target = base / 'chrome-source', base / 'chrome'
            code = base / 'main.c'
            code.write_text('int main(void) { return 0; }\n')
            subprocess.run(['gcc', '-g', str(code), '-o', str(source)], check=True)
            target.write_bytes(b'old working binary')
            receipt = target.with_name('chrome.staging.json')
            receipt.write_text('{"old":true}')
            strip = Path(shutil.which('strip'))
            with mock.patch.object(stage.subprocess, 'run', side_effect=
                                   subprocess.CalledProcessError(1, ['strip'])):
                with self.assertRaises(subprocess.CalledProcessError):
                    stage.stage_binary(source, target, strip)
            self.assertEqual(target.read_bytes(), b'old working binary')
            self.assertEqual(receipt.read_text(), '{"old":true}')
            def edit(_command, **_kwargs):
                source.write_bytes(b'concurrent edit')
            with mock.patch.object(stage.subprocess, 'run', side_effect=edit):
                with self.assertRaisesRegex(RuntimeError, 'input changed'):
                    stage.stage_binary(source, target, strip)
            self.assertEqual(target.read_bytes(), b'old working binary')
            self.assertEqual(receipt.read_text(), '{"old":true}')
            self.assertEqual(sorted(p.name for p in base.iterdir()),
                             ['chrome', 'chrome-source', 'chrome.staging.json', 'main.c'])

    def test_invalid_elf_and_broken_strip_output_never_publish(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            source, target = base / 'source', base / 'esp/chrome'
            code = base / 'main.c'
            code.write_text('int main(void) { return 7; }\n')
            subprocess.run(['gcc', '-g', str(code), '-o', str(source)], check=True)
            original = source.read_bytes()
            strip = Path(shutil.which('strip'))
            stage.stage_binary(source, target, strip)
            receipt = target.with_name('chrome.staging.json')
            before = (target.stat().st_ino, target.read_bytes(), receipt.read_bytes())
            invalid = [b'not an executable', original[:63], original[:100]]
            for offset, replacement in [(4, b'\x01'), (5, b'\x02'), (18, b'\xb7\x00'),
                                        (16, b'\x01\x00'), (56, b'\x00\x00')]:
                changed = bytearray(original)
                changed[offset:offset + len(replacement)] = replacement
                invalid.append(changed)
            for data in invalid:
                source.write_bytes(data)
                with mock.patch.object(stage.subprocess, 'run') as run:
                    with self.assertRaises(ValueError):
                        stage.stage_binary(source, target, strip)
                    run.assert_not_called()
                self.assertEqual((target.stat().st_ino, target.read_bytes(), receipt.read_bytes()), before)
            source.write_bytes(original)
            receipt.write_text('{}')
            def corrupt(command, **_kwargs):
                Path(command[-1]).write_bytes(b'broken stripped output')
            with mock.patch.object(stage.subprocess, 'run', side_effect=corrupt):
                with self.assertRaises(ValueError):
                    stage.stage_binary(source, target, strip)
            self.assertEqual(target.read_bytes(), before[1])
            self.assertEqual(receipt.read_text(), '{}')
            self.assertFalse(list(target.parent.glob('.chrome-stage-*')))


if __name__ == '__main__':
    unittest.main()
