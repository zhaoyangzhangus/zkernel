"""Check content invalidation and failure recovery without running a build."""
import importlib.util
from pathlib import Path
import os
import subprocess
import tempfile
import sys
import time
from unittest import mock

root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('video_cache',root/'tools/nvidia/video-build-cache.py')
cache=importlib.util.module_from_spec(spec);spec.loader.exec_module(cache)
real_run=subprocess.run
with tempfile.TemporaryDirectory(prefix='video-cache-') as directory:
    base=Path(directory);source=base/'input';header=base/'header';output=base/'output';receipt=base/'receipt'
    source.write_text('one');header.write_text('header')
    calls=[]
    def run(command,cwd,check):
        assert check;calls.append((command,cwd))
        if command==['fail']:raise subprocess.CalledProcessError(1,command)
        output.write_text(source.read_text()+header.read_text())
    cache.subprocess.run=run
    def build(command=None,inputs=None):
        return cache.cached_command(command or ['compile'],inputs or [source,header],[output],receipt,cwd=base)
    assert build() and len(calls)==1
    assert not build() and len(calls)==1
    old=source.stat();source.write_text('two');os.utime(source,ns=(old.st_atime_ns,old.st_mtime_ns))
    assert build() and len(calls)==2
    header.write_text('changed')
    assert build() and len(calls)==3
    output.write_text('tampered')
    assert build() and len(calls)==4
    output.unlink()
    assert build() and len(calls)==5
    assert build(['compile','new-flag']) and len(calls)==6
    added=base/'added';added.write_text('new dependency')
    assert build(inputs=[source,header,added]) and len(calls)==7
    before=receipt.read_bytes()
    try:build(['fail']);raise AssertionError('failed command accepted')
    except subprocess.CalledProcessError:pass
    assert receipt.read_bytes()==before
    receipt.write_text('[]')
    assert build() and len(calls)==9
    old_cpath=os.environ.get('CPATH')
    try:
        os.environ['CPATH']=str(base/'different-header-search')
        assert build() and len(calls)==10
    finally:
        if old_cpath is None:os.environ.pop('CPATH',None)
        else:os.environ['CPATH']=old_cpath
    assert build() and len(calls)==11
    header.unlink()
    try:build();raise AssertionError('missing input accepted')
    except FileNotFoundError:pass
    assert len(calls)==11
cache.subprocess.run=real_run
with tempfile.TemporaryDirectory(prefix='video deps with spaces ') as directory:
    base=Path(directory);source=base/'source.c';header=base/'private header.h'
    output=base/'program';receipt=base/'receipt.json'
    header.write_text('#define VALUE 7\n')
    source.write_text('#include "private header.h"\n#include <stdint.h>\nint main(void) { return (int)(uint32_t)VALUE; }\n')
    dependencies=cache.compiler_dependencies(['cc'],[source])
    assert source.resolve() in dependencies and header.resolve() in dependencies
    assert any(p.name=='stdint.h' for p in dependencies), 'System headers must be tracked'
    command=['cc',str(source),'-o',str(output)]
    assert cache.cached_command(command,dependencies,[output],receipt)
    before=output.stat()
    assert not cache.cached_command(command,dependencies,[output],receipt)
    assert output.stat().st_mtime_ns==before.st_mtime_ns
    old=header.stat();header.write_text('#define VALUE 9\n')
    os.utime(header,ns=(old.st_atime_ns,old.st_mtime_ns))
    assert cache.cached_command(command,dependencies,[output],receipt)
    assert subprocess.run([str(output)]).returncode==9
    before=receipt.read_bytes()
    def edit_during_build(*args,**kwargs):
        result=real_run(*args,**kwargs)
        header.write_text('#define VALUE 3\n')
        return result
    with mock.patch.object(cache.subprocess,'run',side_effect=edit_during_build):
        try:
            cache.cached_command([*command,'-g'],dependencies,[output],receipt)
        except RuntimeError as error:
            assert 'input changed' in str(error)
        else:
            raise AssertionError('Concurrent input edit was cached')
    assert receipt.read_bytes()==before
with tempfile.TemporaryDirectory(prefix='video parallel cache ') as directory:
    base=Path(directory);source=base/'source';output=base/'output';receipt=base/'receipt'
    source.write_text('input')
    runner=base/'runner.py'
    runner.write_text('''import importlib.util,sys
from pathlib import Path
spec=importlib.util.spec_from_file_location('cache',sys.argv[1])
cache=importlib.util.module_from_spec(spec);spec.loader.exec_module(cache)
base=Path(sys.argv[2])
command=[sys.executable,str(base/'compile.py')]
cache.cached_command(command,[base/'source'],[base/'output'],base/'receipt')
''')
    (base/'compile.py').write_text('''from pathlib import Path
import os,time
base=Path(__file__).parent
(base/('entered-'+str(os.getpid()))).touch()
deadline=time.monotonic()+5
while not (base/'release').exists():
    if time.monotonic()>deadline:raise TimeoutError('test release missing')
    time.sleep(.01)
(base/'output').write_text((base/'source').read_text())
''')
    command=[sys.executable,str(runner),str(root/'tools/nvidia/video-build-cache.py'),str(base)]
    workers=[]
    try:
        workers.append(subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True))
        deadline=time.monotonic()+3
        while not list(base.glob('entered-*')) and time.monotonic()<deadline:time.sleep(.01)
        assert len(list(base.glob('entered-*')))==1
        workers.append(subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True))
        time.sleep(.25)
        assert workers[1].poll() is None
        assert len(list(base.glob('entered-*')))==1, 'Second caller executed while first owned the phase'
        (base/'release').touch()
        results=[p.communicate(timeout=5) for p in workers]
        assert all(p.returncode==0 for p in workers),results
        assert len(list(base.glob('entered-*')))==1
        assert 'VIDEO_BUILD_REUSED receipt' in results[1][0]
        assert output.read_text()=='input' and receipt.is_file()
    finally:
        (base/'release').touch()
        for p in workers:
            if p.poll() is None:p.kill()
            p.wait()
with tempfile.TemporaryDirectory(prefix='video atomic link ') as directory:
    base=Path(directory);source=base/'source.c';output=base/'program';receipt=base/'receipt'
    source.write_text('int main(void) { return 7; }\n')
    command=['cc',source.name,'-o',output.name]
    def link(extra=()):
        return cache.cached_command([*command,*extra],[source],[output.name],receipt,
                                    cwd=base,atomic_output=True)
    assert link()
    assert subprocess.run([str(output)]).returncode==7
    assert output.stat().st_mode & 0o111
    before=output.stat();contents=output.read_bytes();saved_receipt=receipt.read_bytes()
    assert not link()
    assert output.stat().st_ino==before.st_ino
    # 不同的缓存签名仍可能对应完全相同的数据。
    assert link(['-Wl,--as-needed'])
    assert output.read_bytes()==contents and output.stat().st_ino==before.st_ino
    saved_receipt=receipt.read_bytes()
    def failed_link(invocation,**kwargs):
        Path(invocation[invocation.index('-o')+1]).write_bytes(b'partial')
        raise subprocess.CalledProcessError(1,invocation)
    with mock.patch.object(cache.subprocess,'run',side_effect=failed_link):
        try:link(['-g'])
        except subprocess.CalledProcessError:pass
        else:raise AssertionError('Partial failed link was accepted')
    assert output.read_bytes()==contents and output.stat().st_ino==before.st_ino
    assert receipt.read_bytes()==saved_receipt
    def racing_link(*args,**kwargs):
        result=real_run(*args,**kwargs)
        source.write_text('int main(void) { return 9; }\n')
        return result
    with mock.patch.object(cache.subprocess,'run',side_effect=racing_link):
        try:link(['-g'])
        except RuntimeError:pass
        else:raise AssertionError('Concurrent edit was published')
    assert output.read_bytes()==contents and output.stat().st_ino==before.st_ino
    assert receipt.read_bytes()==saved_receipt
    assert not list(base.glob('.program-link-*')), 'Temporary link output leaked'
    assert link() and subprocess.run([str(output)]).returncode==9
    for invalid in (['cc'],['cc','-o'],['cc','-o','wrong'],['cc','-o',output.name,'-o',output.name]):
        try:cache.cached_command(invalid,[source],[output.name],receipt,cwd=base,atomic_output=True)
        except ValueError:pass
        else:raise AssertionError('Invalid atomic output declaration accepted')
print('Video build cache: invalidation, serialization and atomic link publication passed')
