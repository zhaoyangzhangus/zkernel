set pagination off
set confirm off
python
import gdb, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/chromium').resolve()))
from trace_build import validate_chrome_build
expected = 'Build ID: 5c734622f8e4d5fe'
try:
    validate_chrome_build(expected)
except RuntimeError as error:
    raise gdb.GdbError('Refusing stale Chrome diagnostic: ' + str(error))
end
target remote /tmp/system-keys.sock
python
# 当前构建的符号 + ELF_PIE_BIAS。本脚本用于诊断命中情况，不是计时基准。
# 不会输出 URL、请求头或缓存的响应内容。
class CacheRead(gdb.Breakpoint):
    def __init__(self):
        super().__init__('*0x4bacea40',gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        self.hits,self.positive,self.errors = 0,0,0
    def stop(self):
        result=int(gdb.parse_and_eval('(int)$esi'))
        self.hits+=1
        self.positive+=result>0
        self.errors+=result<0
        print('HTTP_CACHE_READ_COMPLETE result=%d' % result,flush=True)
        if self.positive>=4 or self.hits>=32:
            print('HTTP_CACHE_READ_SUMMARY calls=%d positive=%d errors=%d' %
                  (self.hits,self.positive,self.errors),flush=True)
            gdb.post_event(lambda:gdb.execute('detach'))
            gdb.post_event(lambda:gdb.execute('quit'))
            return True
        return False
CacheRead()
end
continue
