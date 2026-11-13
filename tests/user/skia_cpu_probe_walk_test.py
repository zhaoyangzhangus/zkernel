"""Production read-only Skia probe translation: 4K/2M/1G and failure gates."""
import ast
from pathlib import Path
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
source = (root/'tools/chromium/skia-cpu-probe.py').read_text()
tree = ast.parse(source)
assignment = next(n for n in tree.body if isinstance(n,ast.Assign)
                  and any(isinstance(t,ast.Name) and t.id=='code' for t in n.targets))
payload = ast.parse(assignment.value.right.value)
helper = next(n for n in payload.body if isinstance(n,ast.FunctionDef) and n.name=='read_user')
direct = 0xffff800000000000
va = 0x537940d0
for large_shift in (None,21,30):
    entries = {}
    table = 0x1000
    for shift,next_table in ((39,0x2000),(30,0x3000),(21,0x4000),(12,0x6000)):
        slot = direct+table+((va>>shift)&511)*8
        if shift == large_shift:
            entries[slot] = 0x80000000 | 129 | (1<<63)
            expected = direct+0x80000000+(va&((1<<shift)-1))
            break
        entries[slot] = next_table | 1 | (1<<63)
        table = next_table
    else:
        expected = direct+0x6000+(va&4095)
    reads = []
    def read_memory(address,size):
        reads.append((address,size))
        assert address==expected and size in (4,8)
        return (0x12345678).to_bytes(size,'little')
    env = {'table':0x1000,'u64':entries.__getitem__,
           'memory':SimpleNamespace(read_memory=read_memory),
           'gdb':SimpleNamespace(GdbError=RuntimeError)}
    exec(compile(ast.Module(body=[helper],type_ignores=[]),'<production-skia-reader>','exec'),env)
    assert env['read_user'](va,4)==0x12345678
    assert env['read_user'](va,8)==0x12345678
    assert len(reads)==2
    first_slot=direct+0x1000+((va>>39)&511)*8
    entries[first_slot]=0
    try:env['read_user'](va,4)
    except RuntimeError as error:assert 'Unmapped' in str(error)
    else:raise AssertionError('missing PTE accepted')
    assert len(reads)==2
# 跨页读取符号必须被拒绝，不能静默读取下一帧。
env['u64']=lambda slot: 0x1001
try:env['read_user'](va&~4095|4093,8)
except RuntimeError as error:assert 'alignment' in str(error)
else:raise AssertionError('cross-page read accepted')
assert 'ELF_PIE_BIAS' in source
assert 'Chrome identity changed' in source and 'len(matches)!=1' in source
assert "'gdbserver none'" in source and "command('cont')" in source
print('Skia probe: production 4K/2M/1G walk, NX masking, missing/cross-page rejection and resume gates PASS')
