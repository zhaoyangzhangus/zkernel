"""Exercise the canonical compiler/linker in explicit guest glibc mode."""
import os
import argparse
import shutil
from contextlib import redirect_stdout
import io
from pathlib import Path
import re
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
root = Path(__file__).resolve().parents[2]
source = chromium_source()
wrapper = source/'build/toolchain/native_os/clang'
linker = source/'build/toolchain/native_os/link'
glibc = root/'build/nvidia/host-deps/glibc'
runtime = glibc/'libc6-extract/lib/x86_64-linux-gnu'
dev = glibc/'libc6-dev-extract/usr/lib/x86_64-linux-gnu'
selection = [f'--native-os-root={root}', f'--native-os-glibc-root={glibc}']
environment = dict(os.environ, OS_ROOT='/nonexistent/ambient-root',
                   CHROME_GLIBC_PROBE_LOADER=str(runtime/'ld-linux-x86-64.so.2'),
                   CHROME_GLIBC_PROBE_LIBRARIES=str(runtime))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--publish-guest-probe', action='store_true')
options = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='chrome-glibc-toolchain-') as directory:
    base = Path(directory)
    environment['CHROME_GLIBC_PROBE_FS_ROOT'] = str(base)
    obj, binary = base/'probe.o', base/'probe'
    subprocess.run([str(wrapper), *selection, '-std=c11', '-O2', '-Wall',
                    '-Wextra', '-Werror', '-fPIE', '-c',
                    '-I', str(source/'buildtools/third_party/libc++'),
                    str(root/'tests/user/chrome_glibc_toolchain_probe.c'),
                    '-o', str(obj)], env=environment, check=True, timeout=30)
    link_command = [str(linker), f'--native-os-glibc-root={glibc}',
                    '-nostdlib', '--unwindlib=none', '-Werror', '-pie', '-Wl,-e,_start',
                    '-Wl,-dynamic-linker,/usr/apps/chrome/lib/ld-linux-x86-64.so.2',
                    '-Wl,-rpath,/usr/apps/chrome/lib',
                    '-Wl,--disable-new-dtags', str(dev/'Scrt1.o'), str(dev/'crti.o'),
                    str(obj), '-lc', '-lm', '-lpthread', '-ldl',
                    str(runtime/'ld-linux-x86-64.so.2'),
                    str(source/'third_party/llvm-build/Release+Asserts/lib/clang'/
                        re.search(r'clang_version\s*=\s*"([^"]+)"',
                                  (source/'build/toolchain/toolchain.gni').read_text()).group(1)/
                        'lib/x86_64-unknown-linux-gnu/libclang_rt.builtins.a'),
                    str(dev/'libc_nonshared.a'), str(dev/'crtn.o'),
                    '-o', str(binary)]
    subprocess.run(link_command, env=environment, check=True, timeout=30)
    elf_env = dict(os.environ, LC_ALL='C')
    dynamic = subprocess.check_output(['readelf', '-dW', str(binary)],
                                     env=elf_env, text=True)
    needed = re.findall(r'\(NEEDED\).*Shared library: \[([^\]]+)\]', dynamic)
    assert 'libc.so.6' in needed and 'libsystemc.so.1' not in needed
    assert not any('stdc++' in name for name in needed)
    headers = subprocess.check_output(['readelf', '-lW', str(binary)],
                                     env=elf_env, text=True)
    assert '[Requesting program interpreter: /usr/apps/chrome/lib/ld-linux-x86-64.so.2]' in headers
    stage = (root/'tools/stage-chrome.sh').read_text()
    begin = stage.index('import re\n', stage.index('python3 - "$chrome_source" "$esp_dir"'))
    end = stage.index('\nPY\n', begin)
    settings = base/'args.gn'
    settings.write_text('native_os_runtime = "glibc"\n')
    previous_argv = sys.argv
    try:
        sys.argv = ['gate', str(binary), str(root/'build/esp'), str(settings), str(runtime)]
        with redirect_stdout(io.StringIO()) as output:
            exec(compile(stage[begin:end], 'actual-glibc-staging-gate', 'exec'), {})
        assert output.getvalue().strip() == 'glibc'
    finally:
        sys.argv = previous_argv
    subprocess.run([sys.executable, str(root/'tools/nvidia/vulkan_runtime_abi.py'),
                    '--runtime-dir', str(root/'build/esp/usr/apps/chrome/lib'),
                    str(binary)], check=True, timeout=30)
    # Run with the guest loader explicitly, not the host interpreter. This
    # validates ABI/TLS/dlopen on the build host, NOT native-kernel execution.
    subprocess.run([str(runtime/'ld-linux-x86-64.so.2'), '--library-path',
                    str(runtime), str(binary)], env=environment, check=True, timeout=15)
    probe_log = base/'guest-probe.log'
    probe_command = [str(runtime/'ld-linux-x86-64.so.2'), '--library-path',
                     str(runtime), str(binary)]
    subprocess.run([*probe_command, '--log', str(probe_log)],
                   env=environment, check=True, timeout=15)
    assert probe_log.read_text().splitlines() == [
        'CHROME_GLIBC_START', 'CHROME_GLIBC_SYSINFO_PASS', 'CHROME_GLIBC_CPU_CLOCK_PASS',
        'CHROME_GLIBC_THREAD_TLS_PASS',
        'CHROME_GLIBC_DLOPEN_PASS', 'CHROME_GLIBC_COMPILER_RUNTIME_PASS',
        'CHROME_GLIBC_DIRECTORY_PASS', 'CHROME_GLIBC_SYMLINK_PASS',
        'CHROME_GLIBC_UNLINK_PASS',
        'CHROME_GLIBC_RENAME_PASS',
        'CHROME_GLIBC_RECORD_LOCK_PASS',
        'CHROME_GLIBC_SHARED_MAP_PASS',
        'CHROME_GLIBC_FORK_IPC_PASS',
        'CHROME_GLIBC_EXEC_FD_PASS']
    for arguments in (['--log'], ['--unknown'],
                      ['--log', str(base/'absent-directory/probe.log')]):
        result = subprocess.run([*probe_command, *arguments], env=environment,
                                timeout=15)
        assert result.returncode == 2
    for invalid in ('', '/nonexistent/guest-glibc'):
        result = subprocess.run([str(wrapper), f'--native-os-root={root}',
                                 '--native-os-glibc-root='+invalid,
                                 '-E', '-x', 'c', '/dev/null'],
                                capture_output=True, text=True, timeout=15)
        assert result.returncode != 0 and 'Invalid guest glibc header root' in result.stderr
        result = subprocess.run([str(linker), '--native-os-glibc-root='+invalid,
                                 '-o', str(base/'must-not-exist')],
                                capture_output=True, text=True, timeout=15)
        assert result.returncode != 0 and 'Invalid guest glibc runtime root' in result.stderr
    result = subprocess.run([str(linker), f'--native-os-glibc-root={glibc}',
                             '-lstdc++', '-o', str(base/'must-not-exist')],
                            capture_output=True, text=True, timeout=15)
    assert result.returncode != 0 and 'Unapproved guest library search' in result.stderr
    assert not (base/'must-not-exist').exists()
    if options.publish_guest_probe:
        private_link = [argument.replace('/usr/apps/chrome/lib', '/usr/apps/chrome-glibc-probe/lib')
                        for argument in link_command]
        subprocess.run(private_link, env=environment, check=True, timeout=30)
        destination = root/'build/nvidia/chrome-glibc-probe'
        shutil.copyfile(binary, destination)
        destination.chmod(0o755)
        app = root/'build/esp/usr/apps/chrome-glibc-probe'
        (app/'lib').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(binary, app/'chrome-glibc-probe')
        for name in ('libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0',
                     'librt.so.1', 'ld-linux-x86-64.so.2'):
            shutil.copyfile(runtime/name, app/'lib'/name)
        print('Guest compatibility probe:', app, flush=True)
print('Guest glibc toolchain: real pinned headers/ELF, guest-loader thread/TLS/dlopen, explicit root precedence and host-library rejection passed')
subprocess.run([sys.executable, str(root/'tests/user/chrome_vulkan_compile_test.py'),
                '--runtime', 'glibc'], check=True, timeout=80)
