"""界面文件一次生成命名索引，拒绝平方查重、线性绑定及哈希碰撞。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
loader = (root / 'user/runtime/control_file.c').read_text()
validator = (root / 'include/uapi/control_validation.h').read_text()
validator = validator.replace('#include "control_tree.h"', '#include <uapi/control_tree.h>')
validator = validator.replace('return nodes[a].identifier>nodes[b].identifier;',
    '++sort_comparisons;return nodes[a].identifier>nodes[b].identifier;')
prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned live,allocations,fail_at,sort_comparisons,lookup_steps,duplicate_steps;
static void *counted_malloc(size_t bytes){
 if(++allocations==fail_at)return 0;
 void *p=malloc(bytes);if(p)++live;return p;
}
static void counted_free(void *p){if(p){assert(live);--live;free(p);}}
#define malloc counted_malloc
#define free counted_free
'''
checks = r'''
#undef malloc
#undef free
int main(void){
 char *source=malloc(UI_FILE_MAX_BYTES+1);assert(source);
 size_t length=(size_t)sprintf(source,"<container id='root'>");
 for(unsigned i=1;i<OS_CONTROL_TREE_MAX_NODES;++i)
  length+=(size_t)sprintf(source+length,"<button id='node_%u'/>",i);
 length+=(size_t)sprintf(source+length,"</container>");
 assert(length<=UI_FILE_MAX_BYTES);
 ui_file_document_t d={0};ui_file_error_t error={0};
 assert(ui_file_parse(source,length,&d,&error));
 assert(live==1&&allocations==1&&d.request.count==OS_CONTROL_TREE_MAX_NODES);
 assert(sizeof(*d.identifiers)==2&&d.identifiers==(uint16_t *)(d.nodes+OS_CONTROL_TREE_MAX_NODES));
 assert(!duplicate_steps&&sort_comparisons<32U*OS_CONTROL_TREE_MAX_NODES);
 os_control_node_t *saved=malloc((size_t)d.request.count*sizeof(*saved));assert(saved);
 memcpy(saved,d.nodes,(size_t)d.request.count*sizeof(*saved));
 unsigned lookups=0,before=allocations;
 for(unsigned i=0;i<d.request.count;++i){
  char name[32];if(i)sprintf(name,"node_%u",i);else strcpy(name,"root");
  unsigned steps=lookup_steps;
  assert(ui_file_find(&d,name)==d.nodes+i);
  assert(lookup_steps-steps<=13);++lookups;
  if(i)assert(d.nodes[d.identifiers[i-1]].identifier<d.nodes[d.identifiers[i]].identifier);
 }
 assert(!ui_file_find(&d,"missing")&&!ui_file_find(&d,0)&&!ui_file_find(0,"root"));
 assert(!memcmp(saved,d.nodes,(size_t)d.request.count*sizeof(*saved))&&allocations==before);
 d.nodes[2000].foreground=0x123456;d.nodes[2000].user_data=UINT64_MAX;
 assert(ui_file_find(&d,"node_2000")==d.nodes+2000);
 printf("UI file index: 4096 nodes, sort comparisons=%u, %u bindings <=13 steps each, no lookup allocation\n",
  sort_comparisons,lookups);
 ui_file_destroy(&d);assert(!live&&!d.nodes&&!d.identifiers);free(saved);
 /* 两个不同的合法名字具有相同 FNV-1a ID，不能被视为同一绑定。 */
 const char *collision="<container id='root'><button id='njsrtqgelta'/><button id='ndthsepwhbb'/></container>";
 assert(ui_file_identifier("njsrtqgelta")==ui_file_identifier("ndthsepwhbb"));
 assert(!ui_file_parse(collision,strlen(collision),&d,&error)&&!live&&!d.identifiers);
 const char *duplicate="<container id='root'><button id='same'/><button id='same'/></container>";
 assert(!ui_file_parse(duplicate,strlen(duplicate),&d,&error)&&!live&&!d.identifiers);
 allocations=0;fail_at=1;
 assert(!ui_file_parse(source,length,&d,&error)&&!live&&!d.nodes&&!d.identifiers);
 fail_at=0;ui_file_document_t nonempty={.identifiers=(void *)(uintptr_t)1};
 assert(!ui_file_parse(source,length,&nonempty,&error));
 assert(!ui_file_load("/unused",&nonempty,&error)&&nonempty.identifiers==(void *)(uintptr_t)1);
 assert(!ui_file_find(&d,"root"));free(source);
 puts("UI file index: real hash collision, duplicate IDs, style updates and failure rollback passed");
}
'''


def instrument(code):
    code = code.replace('while(first<last){', 'while(first<last){++lookup_steps;')
    code = code.replace('if(p->document->nodes[i].identifier==node->identifier)',
                        'if((++duplicate_steps,p->document->nodes[i].identifier==node->identifier))')
    return prefix + code + checks


with tempfile.TemporaryDirectory(prefix='control-file-index-') as directory:
    path = Path(directory)
    (path / 'uapi').mkdir()
    (path / 'uapi/control_validation.h').write_text(validator)
    command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               '-I' + str(path), '-I' + str(root / 'user/runtime'), '-I' + str(root / 'include'),
               str(path / 'test.c'), '-o', str(path / 'test')]
    (path / 'test.c').write_text(instrument(loader))
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True)
    old_duplicate = '''for(uint32_t i=0;i<index;++i)if(p->document->nodes[i].identifier==node->identifier)
        return ui_fail(p,"duplicate identifier or hash collision");
    '''
    negative = loader.replace('if(kind==OS_CONTROL_SURFACE&&node->surface!=1U)',
                              old_duplicate + 'if(kind==OS_CONTROL_SURFACE&&node->surface!=1U)')
    assert negative != loader
    (path / 'test.c').write_text(instrument(negative))
    subprocess.run(command, check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True)
    assert result.returncode != 0 and b'duplicate_steps' in result.stderr, result.stderr
    print('Negative control rejected: quadratic per-node duplicate scan')
    start = loader.index('    uint32_t first=0,last=d->request.count;')
    end = loader.index('    return 0;', start)
    negative = loader[:start] + '''    for(uint32_t i=0;i<d->request.count;++i){
        ++lookup_steps;if(d->nodes[i].identifier==id)return d->nodes+i;
    }
''' + loader[end:]
    (path / 'test.c').write_text(instrument(negative))
    subprocess.run(command, check=True)
    result = subprocess.run([str(path / 'test')], capture_output=True)
    assert result.returncode != 0 and b'lookup_steps' in result.stderr, result.stderr
    print('Negative control rejected: linear named binding')
