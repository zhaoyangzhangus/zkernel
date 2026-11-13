"""Content-checked cache for video build phases, not timestamp-only skipping."""
import hashlib
import json
from pathlib import Path
import subprocess
import os
import shlex
import fcntl
import tempfile


def compiler_dependencies(command, sources, cwd=None):
    """Ask the compiler for system and project headers, including spaced paths."""
    dependencies=set()
    for source in sources:
        output=subprocess.check_output([*map(str,command), '-E', '-M',
                                        '-MT', 'video_dependency', str(source)],
                                       cwd=cwd,text=True)
        rule=output.replace('\\\n',' ')
        target, separator, values=rule.partition(':')
        if not separator or target.strip()!='video_dependency':
            raise ValueError('Unexpected compiler dependency output')
        entries=shlex.split(values)
        if not entries:
            raise ValueError('Empty compiler dependency output')
        for value in entries:
            path=Path(value)
            if not path.is_absolute() and cwd is not None:path=Path(cwd)/path
            if not path.is_file():raise FileNotFoundError(path)
            dependencies.add(path.resolve())
    return sorted(dependencies)

def checksum(path):
    digest=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):
            digest.update(chunk)
    return digest.hexdigest()

def cached_command(command, inputs, outputs, receipt, cwd=None, atomic_output=False):
    receipt=Path(receipt)
    receipt.parent.mkdir(parents=True,exist_ok=True)
    # 在检查、执行和发布期间始终使用同一个稳定的锁 inode。
    # 退出时不要删除它：否则等待中的调用者可能会锁住替代 inode，
    # 而另一个调用者仍持有原 inode。
    with receipt.with_suffix(receipt.suffix+'.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        return _cached_command_locked(command,inputs,outputs,receipt,cwd,atomic_output)


def _cached_command_locked(command, inputs, outputs, receipt, cwd=None, atomic_output=False):
    receipt=Path(receipt)
    inputs=list(inputs)
    outputs=list(outputs)
    command=list(map(str,command))
    output_index=None
    if atomic_output:
        if len(outputs)!=1 or command.count('-o')!=1:
            raise ValueError('Atomic phase requires one output and one -o argument')
        output_index=command.index('-o')+1
        def resolved(path):
            path=Path(path)
            return ((Path(cwd)/path) if cwd is not None and not path.is_absolute() else path).resolve()
        if output_index>=len(command) or resolved(command[output_index])!=resolved(outputs[0]):
            raise ValueError('Atomic phase -o does not match its declared output')
        outputs=[resolved(outputs[0])]
    def identity(path):
        info=Path(path).stat()
        return info.st_dev,info.st_ino,info.st_size,info.st_mtime_ns,info.st_ctime_ns
    watched={str(path):identity(path) for path in inputs}
    def validate_inputs():
        if any(identity(path)!=before for path,before in watched.items()):
            raise RuntimeError('Video build input changed during phase')
    signature={'command':list(map(str,command)), 'cwd':str(cwd) if cwd else None,
               'environment':{name:os.environ.get(name) for name in
                              ('CPATH','C_INCLUDE_PATH','LIBRARY_PATH','GCC_EXEC_PREFIX',
                               'COMPILER_PATH','LD_LIBRARY_PATH','LD_RUN_PATH','SOURCE_DATE_EPOCH')},
               'inputs':{str(p):checksum(p) for p in inputs}}
    if atomic_output:signature['atomic_output']=True
    validate_inputs()
    try:
        previous=json.loads(receipt.read_text())
        current={str(p):checksum(p) for p in outputs}
        if previous.get('inputs')==signature and previous.get('outputs')==current:
            validate_inputs()
            print('VIDEO_BUILD_REUSED '+receipt.stem,flush=True)
            return False
    except (OSError,ValueError,AttributeError):
        pass
    if atomic_output:
        target=Path(outputs[0])
        target.parent.mkdir(parents=True,exist_ok=True)
        fd,name=tempfile.mkstemp(prefix='.'+target.name+'-link-',dir=target.parent)
        os.close(fd)
        pending=Path(name)
        try:
            invocation=command.copy()
            invocation[output_index]=str(pending.resolve())
            subprocess.run(invocation,cwd=cwd,check=True)
            validate_inputs()
            if not pending.is_file() or not pending.stat().st_size:
                raise ValueError('Linker did not produce a nonempty output')
            # mkstemp 创建的文件权限为 0600；链接后的共享对象必须能由客户机
            # 以及之后以非特权身份运行的暂存命令使用。
            pending.chmod(0o755)
            # 若可复现链接生成了相同字节，则保留原 inode，
            # 固定权限模式的 FAT/NTFS 挂载也适用。
            if not target.is_file() or checksum(target)!=checksum(pending):
                pending.replace(target)
        finally:
            pending.unlink(missing_ok=True)
    else:
        subprocess.run(command,cwd=cwd,check=True)
        validate_inputs()
    result={'inputs':signature,'outputs':{str(p):checksum(p) for p in outputs}}
    validate_inputs()
    receipt.parent.mkdir(parents=True,exist_ok=True)
    temporary=receipt.with_suffix(receipt.suffix+'.tmp')
    temporary.write_text(json.dumps(result,sort_keys=True))
    temporary.replace(receipt)
    return True
