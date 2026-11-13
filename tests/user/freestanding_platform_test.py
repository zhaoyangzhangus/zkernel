"""Exercise the selected vendor platform through actual build recipes."""
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
cases = (
    ('build/openssl/providers/implementations/rands/seeding/rand_unix.o',
     '-DOPENSSL_SYS_FREESTANDING'),
    ('build/blend2d/blend2d/core/runtime.o', '-DBL_PLATFORM_FREESTANDING'),
)
for target, flag in cases:
    recipe = subprocess.check_output(['make', '-s', '-n', '-B', '-f', 'GNUmakefile',
        target, 'BUILD=build', 'OS_NVIDIA=1', 'OS_DEBUG_SERIAL=1',
        'OS_INTERACTIVE_DESKTOP=1', 'BLEND2D_ENABLE_JIT=0'], cwd=ROOT, text=True)
    line = next(line for line in recipe.splitlines() if ' -c ' in line and ' -o ' + target in line)
    args = shlex.split(line)
    assert flag in args, target
    assert not any('PLATFORM_LITEOS' in arg or 'SYS_LITEOS' in arg for arg in args)
    index = args.index('-o')
    del args[index:index + 2]
    args = [arg for arg in args if arg not in ('-c', '-MMD', '-MP')] + ['-E', '-P']
    output = subprocess.check_output(args, cwd=ROOT, text=True)
    if 'openssl' in target:
        assert 'return getentropy(buf, buflen) == 0 ? (ssize_t)buflen : -1;' in output
    else:
        assert '-DBL_BUILD_NO_JIT' in args
        assert 'bl_runtime_detect_cpu_features' not in output
        assert 'BLRuntimeInitializer bl_runtime_auto_init' not in output
        # 发布构建配方会禁用 JIT。也检查受保护的未来路径，
        # 不修改生产标志，也不启动另一个客户机。
        jit_args = args + ['-UBL_BUILD_NO_JIT']
        output = subprocess.check_output(jit_args, cwd=ROOT, text=True)
        assert re.search(r'features\s*&=\s*~\(BL_RUNTIME_CPU_FEATURE_X86_AVX\s*\|\s*'
                         r'BL_RUNTIME_CPU_FEATURE_X86_AVX2\s*\|\s*'
                         r'BL_RUNTIME_CPU_FEATURE_X86_AVX512\)', output)
        assert 'BLRuntimeInitializer bl_runtime_auto_init' not in output
        # 验证掩码/自动初始化检查能够区分错误的宿主分支。
        hosted = subprocess.check_output([arg for arg in jit_args if arg != flag], cwd=ROOT, text=True)
        assert 'BLRuntimeInitializer bl_runtime_auto_init' in hosted
        assert not re.search(r'features\s*&=\s*~\(BL_RUNTIME_CPU_FEATURE_X86_AVX', hosted)
for file in ('third_party/blend2d/blend2d/core/api-build_p.h',
             'third_party/blend2d/3rdparty/asmjit/asmjit/core/api-build_p.h'):
    source = (ROOT / file).read_text()
    assert 'BL_PLATFORM_LITEOS' not in source
    assert 'defined(BL_PLATFORM_FREESTANDING)' in source
    assert re.search(r'#define\s+(?:BL|ASMJIT)_FILE64_API\(NAME\)\s+NAME\s*$', source, re.M)
for file in ('third_party/openssl/include/openssl/e_os2.h',
             'third_party/openssl/crypto/bn/bn_local.h',
             'third_party/openssl/providers/implementations/rands/seeding/rand_unix.c'):
    assert 'OPENSSL_SYS_LITEOS' not in (ROOT / file).read_text()
print('Freestanding platforms: actual recipes, entropy path, AVX mask, no auto-init and hosted negative control passed')
