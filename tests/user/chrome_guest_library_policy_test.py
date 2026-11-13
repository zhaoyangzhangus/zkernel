"""Execute actual guest/host library-selection blocks with a tiny GN fixture."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chromium_source import chromium_source
root = chromium_source()
gn = root/'buildtools/linux64/gn'


def block(path, beginning):
    text = (root/path).read_text()
    start = text.index(beginning)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


runtime = block('build/config/linux/BUILD.gn', 'if ((!is_chromeos')
base_atomic = block('base/BUILD.gn', 'if (!use_sysroot && (is_android')
libcxx = block('buildtools/third_party/libc++/BUILD.gn', 'if (is_linux && !is_clang')
fontconfig = block('third_party/fontconfig/BUILD.gn', 'if (!is_win &&')
pango = block('ui/base/BUILD.gn', 'if (use_pangocairo &&')
ime_pango = block('ui/base/ime/linux/BUILD.gn', 'if (use_pangocairo &&')
browser_pango = block('content/browser/BUILD.gn', 'if (use_pangocairo &&')
ffmpeg_zlib = block('third_party/ffmpeg/BUILD.gn',
                    'if (current_toolchain == "//build/toolchain/native_os:native_os_x64")')
# 也包含真实的 else 分支。
ffmpeg_text = (root/'third_party/ffmpeg/BUILD.gn').read_text()
ffmpeg_start = ffmpeg_text.index(ffmpeg_zlib)
ffmpeg_else = ffmpeg_text[ffmpeg_start + len(ffmpeg_zlib):]
assert ffmpeg_else.startswith(' else {\n        libs += [ "z" ]\n      }')
ffmpeg_zlib += ' else {\n libs += [ "z" ]\n}'
native = '//build/toolchain/native_os:native_os_x64'
host = '//build/toolchain/fixture:host'
tools = '''tool("cxx") {
  command = "true"
  outputs = [ "{{source_out_dir}}/{{target_output_name}}.{{source_name_part}}.o" ]
}
tool("stamp") { command = "true" }
'''
with tempfile.TemporaryDirectory(prefix='chrome-guest-library-policy-') as directory:
    base = Path(directory)
    files = {
        '.gn': 'buildconfig = "//config.gn"\n',
        'config.gn': '''declare_args() { legacy_system_libs = true }
set_default_toolchain("//build/toolchain/native_os:native_os_x64")
current_cpu = "x64"
is_chromeos = false
is_android = false
is_castos = false
use_sysroot = false
host_toolchain = "//build/toolchain/fixture:host"
is_linux = legacy_system_libs
is_win = !legacy_system_libs
is_clang = !legacy_system_libs
use_custom_libcxx = !legacy_system_libs
use_pangocairo = legacy_system_libs
''',
        'build/toolchain/native_os/BUILD.gn': 'toolchain("native_os_x64") {\n'+tools+'}\n',
        'build/toolchain/fixture/BUILD.gn': 'toolchain("host") {\n'+tools+'}\n',
        'build/config/linux/pangocairo/BUILD.gn': 'config("pangocairo") { libs = [ "pangocairo" ] }\n',
        'probe.cc': 'int fixture;\n',
        'composition_text_util_pango.cc': 'int pango_fixture;\n',
        'composition_text_util_pango.h': '// Fixture header.\n',
        'third_party/zlib/BUILD.gn': 'group("zlib") {}\n',
        'BUILD.gn': (
            'config("runtime") {\n'+runtime+'\n}\n'
            'config("base_atomic") {\n libs = []\n'+base_atomic+'\n}\n'
            'config("libcxx") {\n'+libcxx+'\n}\n'
            'config("fontconfig") {\n'+fontconfig+'\n}\n'
            'source_set("probe") {\n sources = [ "probe.cc" ]\n'
            ' configs = [ ":runtime", ":base_atomic", ":libcxx", ":fontconfig" ]\n'
            +pango+'\n}\n'
            'source_set("ime_probe") {\n sources = [ "probe.cc" ]\n configs = []\n'
            +ime_pango+'\n}\n'
            'source_set("browser_probe") {\n sources = [ "probe.cc" ]\n configs = []\n'
            +browser_pango+'\n}\n'
            'source_set("ffmpeg_probe") {\n sources = [ "probe.cc" ]\n libs = []\n deps = []\n'
            +ffmpeg_zlib+'\n}\n'
            'group("all") { deps = [ ":probe('+native+')", ":probe('+host+')" ] }\n'),
    }
    for name, content in files.items():
        path = base/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    for enabled in (True, False):
        generated = subprocess.run([str(gn), 'gen', 'out', '--threads=1',
                        '--args=legacy_system_libs='+str(enabled).lower()],
                       cwd=base, capture_output=True, text=True, timeout=15)
        assert generated.returncode == 0, generated.stdout + generated.stderr
        for toolchain in (native, host):
            result = subprocess.check_output([str(gn), 'desc', 'out',
                '//:probe('+toolchain+')', 'libs', '--format=json'],
                cwd=base, text=True, timeout=15)
            values = json.loads(result)
            assert isinstance(values, dict) and len(values) == 1, values
            properties = next(iter(values.values()))
            assert set(properties).issubset({'libs'}), properties
            libraries = properties.get('libs', [])
            expected = {'atomic', 'uuid', 'pangocairo'} if enabled and toolchain == host else set()
            assert set(libraries) == expected, (enabled, toolchain, values)
            for target in ('ime_probe', 'browser_probe'):
                result = subprocess.check_output([str(gn), 'desc', 'out',
                    '//:'+target+'('+toolchain+')', 'libs', '--format=json'],
                    cwd=base, text=True, timeout=15)
                properties = next(iter(json.loads(result).values()))
                expected = {'pangocairo'} if enabled and toolchain == host else set()
                assert set(properties.get('libs', [])) == expected, properties
            result = subprocess.check_output([str(gn), 'desc', 'out',
                '//:ffmpeg_probe('+toolchain+')', 'libs', '--format=json'],
                cwd=base, text=True, timeout=15)
            properties = next(iter(json.loads(result).values()))
            assert set(properties.get('libs', [])) == ({'z'} if toolchain == host else set())
            result = subprocess.check_output([str(gn), 'desc', 'out',
                '//:ffmpeg_probe('+toolchain+')', 'deps', '--format=json'],
                cwd=base, text=True, timeout=15)
            properties = next(iter(json.loads(result).values()))
            assert bool(properties.get('deps', [])) == (toolchain == native), properties
print('Guest library policy: real GN evaluates production conditions; native '
      'excludes host libraries, host libraries remain enabled, disabled platform '
      'conditions remain disabled; canonical Chromium graph was not regenerated')
