set pagination off
set confirm off
python
import json
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, 'tools/chromium')
from guest_inspection import run

# Compile the actual private layouts before stopping the guest. No duplicated
# offsets, inferior calls, memory writes or persistent profiling selectors.
with tempfile.TemporaryDirectory(prefix='network-queue-types-') as directory:
    objects = []
    for name, source in (('net', 'kernel/drivers/net/core.c'),
                         ('deferred', 'kernel/irq/deferred.c')):
        target = str(Path(directory) / (name + '.o'))
        subprocess.run(['cc', '-g3', '-ffreestanding', '-DOS_KERNEL_BUILD',
                        '-Iinclude', '-c', source, '-o', target],
                       check=True, timeout=30)
        objects.append(target)

    def snapshot():
        import gdb
        names = ('g_e1000_poll_timing', 'g_deferred', 'g_deferred_slow_work',
                 'g_deferred_slow_work_untracked')
        # Capture real addresses before adding unlinked, type-only objects.
        addresses = {name: int(gdb.parse_and_eval('&' + name)) for name in names}
        cpus_address = int(gdb.parse_and_eval('&g_cpus'))
        for target in objects:
            gdb.execute('add-symbol-file ' + json.dumps(target) + ' 0')
        values = {}
        for name in names:
            expected = (gdb.TYPE_CODE_ARRAY if name == 'g_deferred_slow_work' else
                        gdb.TYPE_CODE_INT if name == 'g_deferred_slow_work_untracked' else
                        gdb.TYPE_CODE_STRUCT)
            symbol = next(s for s in gdb.lookup_static_symbols(name)
                          if s.type.strip_typedefs().code == expected)
            values[name] = gdb.Value(addresses[name]).cast(symbol.type.pointer()).dereference()
        timing = values['g_e1000_poll_timing']
        fields = ('samples', 'total_ns', 'max_ns', 'over_1ms', 'over_10ms', 'over_100ms')
        print('NETWORK_QUEUE_WAIT ' + ' '.join('%s=%d' % (name, int(timing[name])) for name in fields))
        records = values['g_deferred_slow_work']
        first, last = records.type.range()
        for index in range(first, last + 1):
            record = records[index]
            address = int(record['function'])
            if not address:
                continue
            identity = gdb.execute('info symbol %#x' % address, to_string=True).strip()
            print('DEFERRED_SLOW_WORK function=%s ' % identity +
                  ' '.join('%s=%d' % (name, int(record[name])) for name in
                           ('count', 'total_ns', 'max_ns', 'last_start_tsc', 'last_end_tsc')))
        print('DEFERRED_SLOW_WORK_UNTRACKED %d' % int(values['g_deferred_slow_work_untracked']))
        work = values['g_deferred']
        worker = work['worker']
        print('DEFERRED count=%d critical=%d worker_tid=%d state=%d cpu=%d' %
              (int(work['count']), int(work['critical_ready']), int(worker['tid']),
               int(worker['state']), int(worker['owner_cpu'])))
        cpus = gdb.Value(cpus_address).cast(gdb.lookup_type('scheduler_cpu_t').pointer())
        owner = int(worker['owner_cpu'])
        if not 0 <= owner < 256:
            raise RuntimeError('Invalid deferred worker CPU')
        queue = cpus[owner]['queue']
        print('DEFERRED_RUNQUEUE current=%#x worker=%#x nr_running=%d flags=%#x' %
              (int(queue['current']), int(worker.address), int(queue['nr_running']),
               int(worker['sched']['flags'])))
        for cpu in gdb.selected_inferior().threads():
            if cpu.num - 1 != owner:
                continue
            cpu.switch()
            print('DEFERRED_OWNER_CPU_PC ' + gdb.execute('info symbol $pc', to_string=True).strip())

    run(snapshot)
end
