"""Exercise the production staging dependency gate without staging an image."""
from pathlib import Path
from contextlib import redirect_stdout
import io
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT/'tools/stage-chrome.sh').read_text()
begin = source.index('import re\n', source.index('python3 - "$chrome_source" "$esp_dir"'))
end = source.index('\nPY\n', begin)
gate = compile(source[begin:end], 'actual-staging-dependency-gate', 'exec')
with tempfile.TemporaryDirectory(prefix='system-stage-dependencies-') as directory:
    esp = Path(directory)
    (esp/'lib').mkdir()
    for name in ('ld-linux-system.so.1', 'ld-linux-liteos.so.1',
                 'libsystemc.so.1', 'libliteosc.so.1'):
        (esp/'lib'/name).touch()
    compatibility = esp/'usr/apps/chrome/lib'
    compatibility.mkdir(parents=True)
    for name in ('libc.so.6', 'libm.so.6', 'ld-linux-x86-64.so.2'):
        (compatibility/name).touch()
    baseline = esp/'selected-glibc'
    baseline.mkdir()
    for name in ('libc.so.6', 'libm.so.6', 'ld-linux-x86-64.so.2'):
        (baseline/name).touch()
    settings = esp/'args.gn'
    import subprocess
    original_output = subprocess.check_output
    original_argv = sys.argv
    def run(interpreters, dependencies, missing=None, error=False,
            mode=None, graph=None, missing_relative=None, duplicate=False, tamper=None):
        calls = []
        def readelf(argv, **kwargs):
            calls.append(argv)
            assert kwargs['env']['LC_ALL'] == 'C', 'ELF inspection must not depend on host language'
            assert argv[0] == 'readelf'
            if error:
                raise subprocess.CalledProcessError(1, argv)
            if argv[1] == '-lW':
                assert argv[2] == '/actual/chrome'
                return ''.join(f'[Requesting program interpreter: {name}]\n' for name in interpreters)
            assert argv[1] == '-dW'
            names = dependencies if argv[2] == '/actual/chrome' else (graph or {}).get(Path(argv[2]).name, [])
            return ''.join(f'0x1 (NEEDED) Shared library: [{name}]\n' for name in names)
        subprocess.check_output = readelf
        sys.argv = ['gate', '/actual/chrome', str(esp)]
        if mode is not None:
            settings.write_text(f'native_os_runtime = "{mode}"\n' * (2 if duplicate else 1))
            sys.argv.append(str(settings))
            sys.argv.append(str(baseline))
        if tamper:
            (esp/tamper).write_bytes(b'mismatched runtime')
        removed = esp/missing_relative if missing_relative else esp/'lib'/missing if missing else None
        if removed:
            removed.unlink()
        try:
            try:
                with redirect_stdout(io.StringIO()) as output:
                    exec(gate, {})
                assert output.getvalue().strip() == (mode or 'system')
                return None
            except SystemExit as failure:
                return str(failure)
        finally:
            if removed:
                removed.touch()
            subprocess.check_output = original_output
            sys.argv = original_argv
            if tamper:
                (esp/tamper).write_bytes(b'')
    loader, library = 'ld-linux-system.so.1', 'libsystemc.so.1'
    assert run(['/lib/'+loader], [library]) is None
    for missing in (loader, library):
        assert 'Missing actual Chrome runtime dependency:' in run(['/lib/'+loader], [library], missing)
    for libraries in (['libliteosc.so.1'], ['libsystemc.so.1', 'libliteosc.so.1']):
        assert 'retired runtime dependency' in run(['/lib/'+loader], libraries)
    for interpreters in ([], ['/lib/ld-system.so.1'], ['/usr/apps/chrome/lib/ld-linux-x86-64.so.2'],
                         ['/lib/ld-linux-liteos.so.1'],
                         ['/lib/ld-linux-system.so.1', '/lib/ld-linux-liteos.so.1']):
        assert 'Linux-personality interpreter' in run(interpreters, ['libsystemc.so.1'])
    for dependencies in ([], ['libc.so.6'], ['libsystemc.so.1.fake']):
        assert 'does not depend' in run(['/lib/ld-linux-system.so.1'], dependencies)
    assert 'Cannot inspect' in run([], [], error=True)
    assert 'mixes system and glibc' in run(['/lib/'+loader], [library, 'libc.so.6'])
    glibc_interpreter = ['/usr/apps/chrome/lib/ld-linux-x86-64.so.2']
    graph = {'libc.so.6':['ld-linux-x86-64.so.2'], 'libm.so.6':['libc.so.6']}
    assert run(glibc_interpreter, ['libc.so.6', 'libm.so.6'], mode='glibc', graph=graph) is None
    graph['ld-linux-x86-64.so.2'] = ['libc.so.6']
    assert run(glibc_interpreter, ['libc.so.6'], mode='glibc', graph=graph) is None
    assert 'Missing actual' in run(glibc_interpreter, ['libc.so.6'], mode='glibc', graph=graph,
                                  missing_relative='usr/apps/chrome/lib/ld-linux-x86-64.so.2')
    for names in ([], [library], ['libc.so.6', library]):
        assert 'exclusively depend' in run(glibc_interpreter, names, mode='glibc')
    for bad in ('../escape.so', '/host/libc.so.6'):
        assert 'Invalid Chrome runtime dependency' in run(glibc_interpreter, ['libc.so.6', bad], mode='glibc')
    assert 'Missing actual' in run(glibc_interpreter, ['libc.so.6', 'host-only.so'], mode='glibc')
    assert 'Linux-personality interpreter' in run(['/lib/'+loader], ['libc.so.6'], mode='glibc')
    assert 'Invalid or duplicate' in run(glibc_interpreter, ['libc.so.6'], mode='host')
    assert 'Invalid or duplicate' in run(glibc_interpreter, ['libc.so.6'], mode='glibc', duplicate=True)
    assert 'baseline mismatch' in run(glibc_interpreter, ['libc.so.6'], mode='glibc', graph=graph,
                                     tamper='usr/apps/chrome/lib/libc.so.6')
    assert 'interpreter does not match' in run(glibc_interpreter, ['libc.so.6'], mode='glibc', graph=graph,
                                             tamper='usr/apps/chrome/lib/ld-linux-x86-64.so.2')
print('Actual Chrome staging gate: current runtime, retired/mixed dependencies rejected, missing dependencies, wrong/duplicate interpreter and inspection failures passed')

begin = source.index('import re\n', source.index('chrome_gpu_profile="$(python3'))
end = source.index('\nPY\n', begin)
profile_gate = compile(source[begin:end], 'actual-Chrome-GPU-profile-selection', 'exec')
with tempfile.TemporaryDirectory(prefix='chrome-gpu-profile-') as directory:
    settings = Path(directory)/'args.gn'
    previous_argv = sys.argv
    def profile(text, runtime):
        settings.write_text(text)
        try:
            sys.argv = ['profile', str(settings), runtime]
            with redirect_stdout(io.StringIO()) as output:
                try:
                    exec(profile_gate, {})
                except SystemExit as failure:
                    return 'error:'+str(failure)
            return output.getvalue().strip()
        finally:
            sys.argv = previous_argv
    for mode in ('system', 'glibc'):
        for text in ('', 'enable_vulkan = false\n', '# enable_vulkan = true\n'):
            assert profile(text, mode) == 'software'
    for text in ('enable_vulkan = true\n', ' enable_vulkan = true # verified backend\n'):
        assert profile(text, 'glibc') == 'vulkan'
        assert 'driver-compatible runtime' in profile(text, 'system')
    for text in ('enable_vulkan = 1\n', 'enable_vulkan = true\nenable_vulkan = false\n',
                 'enable_vulkan = "true"\n'):
        assert 'Invalid or duplicate' in profile(text, 'glibc')
    for mode in ('software', 'vulkan', 'staging'):
        assert (ROOT/f'assets/chrome/gpu-{mode}.conf').read_bytes() == (mode+'\n').encode()
    # The transaction fences must surround every browser/resource write.
    block = source.index('install_data "$project_root/assets/chrome/gpu-staging.conf"')
    launcher = source.index('install_executable "$launcher_source" "$stage_dir/chrome-start"')
    binary = source.index('python3 "$script_dir/chromium/stage-binary.py"')
    resources = source.index('for directory in locales resources MEIPreload; do')
    commit = source.index('install_data "$project_root/assets/chrome/gpu-$chrome_gpu_profile.conf"')
    assert block < launcher < binary < resources < commit < source.index('manifest="$build_dir/chrome-staging.manifest"')
print('Chrome GPU profile: explicit graph/runtime selection, hardware/software switch consistency and interrupted-stage fencing passed')
