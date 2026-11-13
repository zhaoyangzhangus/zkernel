"""Test real single-source host rules, dependency files and toolchain stamps."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'makefile').read_text() + '\n' + (ROOT / 'tools/host-tests.mk').read_text()
names = ['mm-api-test.exe', 'primitives-test.exe', 'header-sanity.exe',
         'abi-sanity.exe', 'frame-trace-disabled-test.exe', 'frame-trace-enabled-test.exe']
stamp = '$(BUILD)/.host-toolchain:' + source.split(
    '$(BUILD)/.host-toolchain:', 1)[1].split('$(BUILD)/build-id.exe:', 1)[0]
macro = 'define HOST_TEST_LINK\n' + source.split('define HOST_TEST_LINK\n', 1)[1].split('endef', 1)[0] + 'endef\n'
rules = []
for name in names:
    marker = '$(BUILD)/' + name + ':'
    rules.append(marker + source.split(marker, 1)[1].split('\n\n', 1)[0])
with tempfile.TemporaryDirectory(prefix='liteos-host-test-cache-') as directory:
    base = Path(directory)
    shutil.copytree(ROOT / 'include', base / 'include')
    for rule in rules:
        input_name = rule.split(':', 1)[1].split()[0]
        destination = base / input_name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / input_name, destination)
    targets = ' '.join('build/' + name for name in names)
    module = base / 'tools/host-tests.mk'
    module.write_text((ROOT / 'tools/host-tests.mk').read_text())
    (base / 'makefile').write_text(
        'BUILD=build\nHOSTCC=gcc\n.PHONY: FORCE all\nall: ' + targets +
        '\nFORCE:\nbuild:\n\tmkdir -p $@\n' + stamp + macro + '\n\n'.join(rules) +
        '\n' + targets + ': build/.host-toolchain makefile tools/host-tests.mk\n'
        '-include $(wildcard build/*.exe.d)\n')
    def run(*args):
        result = subprocess.run(['make', '-j4', 'all', *args], cwd=base,
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
    def identities():
        return {name: ((base / 'build' / name).stat().st_ino,
                       (base / 'build' / name).stat().st_mtime_ns) for name in names}
    run()
    before = identities()
    run()
    assert identities() == before
    header_name = 'include/kernel/bitmap.h'
    header = base / header_name
    header.write_text(header.read_text() + '\n/* dependency edit */\n')
    expected = {name for name in names if header_name in
                (base / 'build' / (name + '.d')).read_text().replace('\\\n', '')}
    assert 'primitives-test.exe' in expected
    run()
    changed = {name for name, identity in identities().items() if before[name] != identity}
    assert changed == expected, (changed, expected)
    before = identities()
    run('HOSTCC=gcc -DHOST_VARIANT=1')
    assert all(identity != before[name] for name, identity in identities().items())
    before = identities()
    run('HOSTCC=gcc -DHOST_VARIANT=1')
    assert identities() == before
    module.write_text(module.read_text() + '\n# rule module edit\n')
    run('HOSTCC=gcc -DHOST_VARIANT=1')
    assert all(identity != before[name] for name, identity in identities().items())
    for name in names:
        subprocess.run([str(base / 'build' / name)], check=True, capture_output=True)
print('Host tests: six real rules, no-op cache, precise header invalidation and toolchain change passed')
