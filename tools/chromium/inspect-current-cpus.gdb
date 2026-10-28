set pagination off
set confirm off
python
import sys
sys.path.insert(0, 'tools/chromium')
from guest_inspection import run

def snapshot():
    import gdb
    cpus = gdb.Value(int(gdb.parse_and_eval('&g_cpus'))).cast(
        gdb.lookup_type('scheduler_cpu_t').pointer())
    for cpu in gdb.selected_inferior().threads():
        cpu.switch()
        print('CPU number=%d pc=%#x' % (cpu.num, int(gdb.parse_and_eval('$pc'))))
        current = cpus[cpu.num - 1]['queue']['current']
        if int(current):
            thread = current.dereference()
            print('CURRENT tid=%d state=%d' % (int(thread['tid']), int(thread['state'])))
            if int(thread['process']):
                process = thread['process'].dereference()
                print('PROCESS pid=%d name=%s' % (int(process['pid']), process['name'].string()))
        gdb.execute('bt 12')

run(snapshot)
end
