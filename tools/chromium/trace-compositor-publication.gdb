set pagination off
set confirm off
python
import gdb,hashlib,time,json
with open('build/qemu-running-kernel.elf','rb') as source:
    if hashlib.sha256(source.read()).hexdigest()!='79818e5af630e87501ba0452be7aff32fba7f96f06e0c9d2ce9615773574297e':
        raise gdb.GdbError('Refusing mismatched compositor symbols')
end
target remote /tmp/system-keys.sock
python
# 捕获一个大型 Chrome PRESENT 快照，使用四个入口地址。宿主诊断区间包含硬件暂停开销；
# 回调不会删除仍有效的断点。
started=None
worker=None
def current():
    cpu=gdb.selected_thread().num-1
    if not 0<=cpu<int(gdb.parse_and_eval('g_cpu_count')):
        raise gdb.GdbError('Unexpected vCPU')
    return gdb.parse_and_eval('g_cpus[%d].queue.current'%cpu)
class Stage(gdb.Breakpoint):
    def __init__(self,name,label):
        # 源码级函数断点可能解析到内联实例并占用多个硬件槽；应使用单一入口地址。
        address=int(gdb.parse_and_eval('&'+name))
        super().__init__('*%#x'%address,gdb.BP_HARDWARE_BREAKPOINT,internal=True)
        self.label=label
    def stop(self):
        global started,worker
        if started is None:
            if self.label!='PLAN':return False
            snapshot=gdb.parse_and_eval('g_compositor_snapshot')
            count=int(snapshot['damage_count'])
            if not 0<count<=int(gdb.parse_and_eval('sizeof(g_compositor_snapshot.damage_rects)/sizeof(Rect)')):
                return False
            pixels=0
            for index in range(count):
                r=snapshot['damage_rects'][index]
                pixels+=max(0,int(r['x1'])-int(r['x0']))*max(0,int(r['y1'])-int(r['y0']))
            if pixels<100000:return False
            windows=[]
            for index in range(int(snapshot['window_count'])):
                view=snapshot['windows'][index];ref=view['reference'].dereference()
                if not int(ref['owner']):continue
                process=ref['owner'].dereference()
                if process['name'].string().rsplit('/',1)[-1]!='chrome':continue
                if int(view['present_sequence'])<=int(ref['compositor_presented_sequence']):continue
                windows.append({'pid':int(process['pid']),'window':int(view['identifier']),
                                'size':[int(view['width']),int(view['height'])]})
            if not windows:return False
            started=time.monotonic();worker=int(current())
            print('COMPOSITOR_SNAPSHOT '+json.dumps({'damage_pixels_sum':pixels,'windows':windows,
                  'worker_thread':hex(worker),'worker_tid':int(current().dereference()['tid'])}),flush=True)
        if int(current())!=worker:return False
        print('COMPOSITOR_STAGE '+json.dumps({'stage':self.label,'diagnostic_host_ms':(time.monotonic()-started)*1000}),flush=True)
        if self.label!='FINISH':return False
        gdb.post_event(lambda:gdb.execute('detach'))
        gdb.post_event(lambda:gdb.execute('quit'))
        return True
Stage('compositor_snapshot_plan','PLAN')
Stage('compositor_render_snapshot','RENDER')
Stage('compositor_commit_snapshot','PUBLISH')
Stage('compositor_snapshot_finish','FINISH')
end
continue
