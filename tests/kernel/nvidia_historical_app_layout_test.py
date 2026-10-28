"""Historical Windows source gates and private bundle mapping, with no host cache I/O."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/nvidia'
spec = importlib.util.spec_from_file_location('historical_checker', TOOLS / 'check_userspace_59584.py')
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)
APPS = {7: 'p7-glibc', 8: 'p8-frontend', 9: 'p9-userspace-load',
        10: 'p10-egl-context', 11: 'p11-triangle', 12: 'p12-window'}


class HistoricalSourceGate(unittest.TestCase):
    def test_build_stage_gate_agree(self):
        for n, app in APPS.items():
            build, stage, gate = ((TOOLS / f'phase{n}_{kind}.ps1').read_text()
                                  for kind in ('build', 'stage', 'gate'))
            self.assertIn(f'-dynamic-linker,/usr/apps/{app}/lib/ld-linux-x86-64.so.2', build)
            self.assertIn(f'-rpath,/usr/apps/{app}/lib', build)
            self.assertIn(f"Assert-AppInterpreter $testBinaryPath '{app}'", stage)
            self.assertIn(f'esp\\usr\\apps\\{app}\\lib\\ld-linux-x86-64.so.2', gate)
            self.assertIn('Assert-AppGuestLayout', stage + gate)
            if n >= 9:
                self.assertIn(f"$espRoot '{app}'", stage)
                self.assertIn('-AppRoot $appRoot', gate)
                self.assertIn('Test-LegacyGuestPath $relative', stage)
                self.assertNotIn('phase10_glvnd_10_nvidia.json', stage)
            self.assertNotIn('/compat/linux', build + stage + gate)
            self.assertNotIn('/lib64/', build + stage + gate)

    def test_host_defaults_and_manifest_preserved(self):
        for name in ('import_userspace_59584.ps1', 'check_userspace_59584.ps1',
                     'phase9_build.ps1', 'phase9_stage.ps1'):
            source = (TOOLS / name).read_text()
            self.assertIn('build/nvidia/userspace', source)
            self.assertNotIn('phase9/compat', source)
        source = (TOOLS / 'import_userspace_59584.ps1').read_text()
        self.assertIn("[string]$Manifest = 'build/nvidia/userspace-595.84.json'", source)
        self.assertIn('$manifestPath = Resolve-RepoPath $Manifest', source)
        self.assertIn("'lib/x86_64-linux-gnu/libEGL.so.1'", source)

    def test_cube_and_inheritance(self):
        for script in TOOLS.glob('*.ps1'):
            source = script.read_text()
            if 'function Copy-DirectoryContents(' in source:
                self.assertIn('Test-LegacyGuestPath $relative', source, script.name)
                self.assertIn('Assert-AppGuestLayout $Target', source, script.name)
        self.assertNotIn("Copy-Item -Path (Join-Path $systemEspRoot '*')",
                         (TOOLS / 'phase4_build.ps1').read_text())
        for name in ('cube_demo_build.ps1', 'cube_desktop_build.ps1'):
            source = (TOOLS / name).read_text()
            self.assertIn('-dynamic-linker,/usr/apps/cube/lib/ld-linux-x86-64.so.2', source)
            self.assertIn('-rpath,/usr/apps/cube/lib', source)
            self.assertIn(" 'cube'", source)
            self.assertIn('Test-LegacyGuestPath $relative', source)
            self.assertIn('usr\\apps\\cube\\cube', source)
            self.assertNotIn('/compat/linux', source)
        deploy = (TOOLS / 'cube_desktop_deploy.ps1').read_text()
        self.assertIn('Assert-AppGuestLayout $espRoot', deploy)
        self.assertIn('Test-LegacyGuestPath $relative', deploy)
        self.assertNotIn("'sbin\\cube'", deploy)
        helper = (TOOLS / 'app-layout.ps1').read_text()
        self.assertIn("'lib\\x86_64-linux-gnu'", helper)
        self.assertIn("'licenses\\NVIDIA-LICENSE'", helper)
        self.assertIn("/lib/libEGL_nvidia.so.0", helper)
        self.assertIn('rebuild in a clean ESP', helper)

    def test_c_probe_roots_and_compile(self):
        p10 = (TOOLS / 'phase10_egl_context_test.c').read_text()
        self.assertIn('/usr/apps/p10-egl-context/egl/10_nvidia.json', p10)
        self.assertIn('/usr/apps/p10-egl-context/lib/libEGL.so.1', p10)
        p11 = (TOOLS / 'phase11_triangle_test.c').read_text()
        self.assertIn('#ifndef P11_APP_ROOT', p11)
        self.assertIn('P11_APP_ROOT "/egl/10_nvidia.json"', p11)
        self.assertIn('P11_APP_ROOT "/lib/libEGL.so.1"', p11)
        self.assertIn('#define P11_APP_ROOT "/usr/apps/p12-window"',
                      (TOOLS / 'phase12_window_test.c').read_text())
        for source in ('phase7_glibc_test.c', 'phase8_frontend_test.c',
                       'phase9_userspace_load_test.c', 'phase10_egl_context_test.c',
                       'phase11_triangle_test.c', 'phase12_window_test.c'):
            subprocess.run(['cc', '-std=gnu11', '-D_GNU_SOURCE', '-Wall', '-Wextra',
                            '-Werror', '-fsyntax-only', '-I' + str(ROOT / 'include'),
                            str(TOOLS / source)], check=True)


class PrivateBundleValidation(unittest.TestCase):
    def test_remapped_hashes_aliases_manifests_and_negative_controls(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            # Independent stand-ins; never inspect build/nvidia or the real cache.
            fixture = work / 'fixture.c'
            fixture.write_text('int fixture(void) { return 42; }\n')
            elf = work / 'fixture.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-nostdlib', str(fixture),
                            '-o', str(elf)], check=True)
            payload = elf.read_bytes()
            names = ('ld-linux-x86-64.so.2', 'libc.so.6', 'libEGL.so.1.1.0',
                     'libEGL_nvidia.so.595.84', 'libOpenGL.so.0', 'libnvidia-allocator.so.595.84')
            aliases = {'libEGL.so.1': names[2], 'libEGL.so': names[2],
                       'libEGL_nvidia.so.0': names[3], 'libOpenGL.so': names[4],
                       'libnvidia-allocator.so.1': names[5], 'libnvidia-allocator.so': names[5]}
            for app in (*APPS.values(), 'cube'):
                root = work / app
                (root / 'lib').mkdir(parents=True)
                (root / 'egl').mkdir()
                (root / 'licenses').mkdir()
                (root / 'licenses/NVIDIA-LICENSE').write_text('fixture license')
                files = []
                for name in names:
                    path = root / 'lib' / name
                    path.write_bytes(payload)
                    files.append(dict(target='lib/x86_64-linux-gnu/' + name,
                                      size=path.stat().st_size, sha256=checker.sha256(path)))
                entries = []
                for alias, target in aliases.items():
                    (root / 'lib' / alias).write_bytes((root / 'lib' / target).read_bytes())
                    entries.append(dict(alias='lib/x86_64-linux-gnu/' + alias,
                                        target='lib/x86_64-linux-gnu/' + target))
                # Host-only entries are intentionally absent from the guest bundle.
                files.append(dict(target='lib64/ld-linux-x86-64.so.2', size=9, sha256='host-only'))
                files.append(dict(target='usr/share/glvnd/egl_vendor.d/10_nvidia.json', size=0, sha256='host-only'))
                manifest = work / 'userspace-595.84.json'
                manifest.write_text(json.dumps(dict(release='595.84', files=files, aliases=entries)), encoding='utf-8-sig')
                vendor_path = root / 'egl/10_nvidia.json'
                vendor = dict(file_format_version='1.0.0', ICD=dict(library_path=f'/usr/apps/{app}/lib/libEGL_nvidia.so.0'))
                vendor_path.write_text(json.dumps(vendor))
                checker.verify_app(root, manifest)
                completed = subprocess.run([sys.executable, str(TOOLS / 'check_userspace_59584.py'),
                                            '--app-root', str(root), '--manifest', str(manifest)],
                                           capture_output=True, text=True, check=True)
                self.assertIn('DYNAMIC_SYMBOL_GATE=PASS', completed.stdout)
                bad_vendor = copy.deepcopy(vendor)
                bad_vendor['ICD']['library_path'] = '/usr/apps/wrong/lib/libEGL_nvidia.so.0'
                vendor_path.write_text(json.dumps(bad_vendor))
                with self.assertRaisesRegex(checker.CheckError, 'app-private ICD'):
                    checker.verify_app(root, manifest)
                vendor_path.write_text(json.dumps(vendor))
                alias = root / 'lib/libEGL.so'
                alias.write_bytes(b'corrupt')
                with self.assertRaisesRegex(checker.CheckError, 'alias mismatch'):
                    checker.verify_app(root, manifest)
                alias.write_bytes((root / 'lib/libEGL.so.1.1.0').read_bytes())
                (root / 'lib/libc.so.6').write_bytes(b'corrupt')
                with self.assertRaisesRegex(checker.CheckError, 'library mismatch'):
                    checker.verify_app(root, manifest)

    def test_elf_checker_on_independent_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'fixture.c'
            source.write_text('int fixture(void) { return 42; }\n')
            subprocess.run(['cc', '-shared', '-fPIC', '-nostdlib', str(source),
                            '-o', str(root / 'libfixture.so')], check=True)
            self.assertEqual(checker.verify_elves(root), 1)


if __name__ == '__main__':
    unittest.main()
