"""Bounded read-only snapshot of Chrome's HTTP cache in the canonical guest."""
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(root / 'tools'))
from qmp_client import QMPClient
code = r'''
import gdb,json,sqlite3,struct,zlib
files=[];directories=[]
capacity=int(gdb.parse_and_eval('g_vfs_node_capacity'))
if not 0 <= capacity <= 32768:
    raise gdb.GdbError('Unexpected VFS registry size; refusing unbounded inspection')
nodes=gdb.parse_and_eval('g_vfs_nodes')
kind=gdb.lookup_type('vfs_backend_node_t').pointer()
for index in range(capacity):
    if not int(nodes[index]):continue
    vnode=nodes[index].dereference()
    if not int(vnode['fs_private']):continue
    backend=vnode['fs_private'].cast(kind).dereference()
    path=backend['path'].string()
    if not path.startswith('/tmp/chrome-data/') or '/Cache/' not in path:continue
    if bool(backend['directory']):
        directories.append(path)
    else:
        record={'path':path,'size':int(vnode['size']),
                      'backend_size':int(backend['size']),
                      'capacity':int(backend['data_capacity'])}
        if path.rsplit('/',1)[-1] in ('sqldb0','sqldb1','sqldb2') and int(backend['data']):
            size=int(backend['size'])
            if 0<size<=16*1024*1024:
                db=sqlite3.connect(':memory:')
                try:
                    db.deserialize(bytes(gdb.selected_inferior().read_memory(int(backend['data']),size)))
                    tables={}
                    for (name,) in db.execute("SELECT name FROM sqlite_master WHERE type='table'"):
                        quoted='"'+name.replace('"','""')+'"'
                        tables[name]=db.execute('SELECT COUNT(*) FROM '+quoted).fetchone()[0]
                    record['sqlite_rows']=tables
                    record['sqlite_quick_check']=[row[0] for row in db.execute('PRAGMA quick_check')]
                    # Chromium CalculateCheckSum 会按网络字节序追加哈希值的低 32 位。
                    # 只检查快照，绝不修改或“修复”客户机缓存字节。正在进行的事务可能会
                    # 暂时导致原始数据库快照不一致。
                    columns={row[1] for row in db.execute('PRAGMA table_info(resources)')}
                    if {'res_id','head','check_sum','cache_key_hash'} <= columns:
                        def checksum(data,key_hash):
                            return zlib.crc32(struct.pack('>I',key_hash & 0xffffffff),
                                              zlib.crc32(data)) & 0xffffffff
                        checks={'headers_checked':0,'blobs_checked':0,
                                'bad_headers':[],'bad_blobs':[], 'partial':False}
                        rows=list(db.execute('SELECT res_id,head,check_sum,cache_key_hash '
                                             'FROM resources WHERE head IS NOT NULL LIMIT 8193'))
                        checks['partial']=len(rows)>8192
                        for res_id,data,expected,key_hash in rows[:8192]:
                            checks['headers_checked']+=1
                            if checksum(data,key_hash) != expected & 0xffffffff:
                                checks['bad_headers'].append(res_id)
                        columns={row[1] for row in db.execute('PRAGMA table_info(blobs)')}
                        if {'blob_id','res_id','blob','check_sum'} <= columns:
                            rows=list(db.execute('SELECT b.blob_id,b.blob,b.check_sum,r.cache_key_hash '
                                'FROM blobs b JOIN resources r USING(res_id) LIMIT 8193'))
                            checks['partial'] |= len(rows)>8192
                            for blob_id,data,expected,key_hash in rows[:8192]:
                                checks['blobs_checked']+=1
                                if checksum(data,key_hash) != expected & 0xffffffff:
                                    checks['bad_blobs'].append(blob_id)
                        record['sqlite_checksum_snapshot']=checks
                except sqlite3.DatabaseError as error:
                    record['sqlite_snapshot_error']=str(error)
                finally:
                    db.close()
        files.append(record)
print('HTTP_CACHE '+json.dumps({'files':files,'directories':directories,
      'file_count':len(files),'total_bytes':sum(f['size'] for f in files)}))
'''
qmp = QMPClient()
def command(name, arguments=None):
    return qmp.command(name, arguments)

assert command('query-status')['running'], 'Do not inspect an externally stopped guest'
command('human-monitor-command',{'command-line':'gdbserver unix:/tmp/system-cache.sock,server=on,wait=off'})
try:
    result=subprocess.run(['gdb','-q','-nx','-batch',str(root/'build/qemu-running-kernel.elf'),
                    '-ex','target remote /tmp/system-cache.sock',
                    '-ex','python exec('+repr(code)+')','-ex','detach'],
                   cwd=root,check=True,timeout=15,capture_output=True,text=True)
    print(result.stdout,end='')
    matches=[line for line in result.stdout.splitlines() if line.startswith('HTTP_CACHE ')]
    if len(matches)!=1 or any(marker in result.stdout+result.stderr for marker in
                            ('Python Exception','Error occurred in Python','Traceback')):
        raise RuntimeError('Cache snapshot failed: '+result.stderr)
    json.loads(matches[0][len('HTTP_CACHE '):])
finally:
    command('human-monitor-command',{'command-line':'gdbserver none'})
    command('cont')
    assert command('query-status')['running']
