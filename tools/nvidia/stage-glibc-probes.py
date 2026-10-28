#!/usr/bin/env python3
"""从固定 SDK 重建网络诊断程序及其私有库，避免搬入过期加载路径。"""
import importlib.util
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('probe_stage', root/'tools/nvidia/stage-vulkan.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)
runtime = root/'build/nvidia/host-deps/glibc/libc6-extract/lib/x86_64-linux-gnu'
catalog = {path.name: path for path in runtime.glob('*.so*') if path.is_file()}
app = 'tcp-wake-probe'
out = root/'build/nvidia/runtime'/app
command = ['python3', str(root/'tools/nvidia/video-baseline-cc.py'), '--app-name', app,
           '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
           str(root/'tests/user/tcp_loopback_wake_probe.c'), '-o', str(out)+'.tmp']
subprocess.run(command, check=True)
stage.stage.builder.publish_binary(str(out)+'.tmp', out)
selected = stage.closure(catalog, stage.check.Elf64(out).needed())
stage.abi.check(runtime, [out, *selected.values()])
target = root/'build/esp/usr/apps'/app
for name, path in selected.items():
    stage.stage.copy_changed(path, target/'lib'/name)
stage.stage.copy_changed(runtime/'ld-linux-x86-64.so.2', target/'lib/ld-linux-x86-64.so.2')
stage.stage.copy_changed(out, target/app)
print('GLIBC_PROBES_STAGED', flush=True)
