"""Exercise recursive Vulkan deployment closure using actual linked ELF files."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('stage_vulkan', ROOT/'tools/nvidia/stage-vulkan.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)
with tempfile.TemporaryDirectory(prefix='vulkan-stage-') as directory:
    base = Path(directory)
    source = base/'fixture.c'
    source.write_text('int fixture(void) { return 7; }\n')
    leaf, root = base/'libleaf.so.1', base/'libroot.so.1'
    def link(output, dependencies):
        subprocess.run(['cc', '-shared', '-fPIC', '-nostdlib', str(source),
                        '-Wl,-soname,'+output.name, '-Wl,--no-as-needed',
                        *map(str, dependencies), '-o', str(output)], check=True)
    link(leaf, [])
    link(root, [leaf])
    catalog = {root.name: root, leaf.name: leaf}
    assert stage.closure(catalog, [root.name]) == catalog
    # dlopen modules must be included even though neither root references them.
    runtime_catalog = dict(catalog, **{'libvulkan.so.1': root,
                                     'libGLX_nvidia.so.0': root,
                                     'libEGL.so.1': root,
                                     'libEGL_nvidia.so.0': root})
    try:
        stage.vulkan_closure(runtime_catalog, base)
        raise AssertionError('accepted missing NVIDIA dlopen module')
    except RuntimeError as error:
        assert stage.NVIDIA_VULKAN_MODULES[0] in str(error)
    for name in stage.NVIDIA_VULKAN_MODULES:
        link(base/name, [leaf])
    selected = stage.vulkan_closure(runtime_catalog, base)
    for name in stage.NVIDIA_VULKAN_MODULES:
        assert selected[name] == base/name
    assert selected[leaf.name] == leaf
    for name in ('libEGL.so.1', 'libEGL_nvidia.so.0'):
        assert name in selected
        incomplete = dict(runtime_catalog)
        del incomplete[name]
        try:
            stage.vulkan_closure(incomplete, base)
            raise AssertionError('accepted missing EGL dynamic dependency')
        except RuntimeError as error:
            assert name in str(error)
    assert stage.closure(catalog, [root.name, root.name]) == catalog
    try:
        stage.closure({root.name: root}, [root.name])
        raise AssertionError('accepted missing transitive dependency')
    except RuntimeError as error:
        assert leaf.name in str(error)
    # Re-link leaf to form a genuine ELF dependency cycle.
    link(leaf, [root])
    assert stage.closure(catalog, [root.name]) == catalog
print('Vulkan staging closure: actual ELF recursion, duplicate roots, cycles and '
      'missing transitive dependency rejection passed')
