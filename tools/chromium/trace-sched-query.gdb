# 针对实现调度器查询的确切内核设置只读返回探测点。
set pagination off
set confirm off
python
import gdb,hashlib,json
from pathlib import Path
expected='ab36f60fc3b046c272f0bef991a68827bcac21e07a8bf7033320b69f44de0b31'
for path in ('build/qemu-running-kernel.elf','build/esp/EFI/OS/kernel.elf'):
    if hashlib.sha256(Path(path).read_bytes()).hexdigest()!=expected:
        raise gdb.GdbError('Stale scheduler query probe offsets')
gdb.execute('target remote /tmp/system-sched-query.sock')
counts={'parameter':0,'policy':0}
errors=[]
class QueryReturn(gdb.Breakpoint):
    def stop(self):
        result=int(gdb.parse_and_eval('$rax')) & ((1<<64)-1)
        if result & (1<<63):result-=1<<64
        kind='policy' if int(gdb.parse_and_eval('$rbx')) & 255 else 'parameter'
        counts[kind]+=1
        print('SCHED_QUERY_RETURN '+json.dumps(dict(kind=kind,result=result)),flush=True)
        if result<0:errors.append(dict(kind=kind,result=result))
        return sum(counts.values())>=8
address=int(gdb.parse_and_eval('&linux_sched_read'))
QueryReturn('*%#x'%(address+0xa4),type=gdb.BP_HARDWARE_BREAKPOINT)
print('SCHED_QUERY_PROBE_READY',flush=True)
end
continue
python
print('SCHED_QUERY_RESULT '+json.dumps(dict(counts=counts,errors=errors)),flush=True)
if not all(counts.values()) or sum(counts.values())<8 or errors:
    raise gdb.GdbError('Incomplete or failed live scheduler query verification')
end
delete breakpoints
detach
quit
