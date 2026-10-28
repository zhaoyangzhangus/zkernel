"""Run production snapshot planning with pruning and sparse window compaction."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/graphics/compositor.c').read_text()
start = source.index('void compositor_snapshot_plan(void)')
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[start:end]
release_start = source.index('static void compositor_release_window_snapshot(')
release = source[release_start:source.index('\n}', release_start) + 2]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define WINDOW_DAMAGE_MAX_SNAPSHOT_RECTS 16
typedef struct { unsigned x0,y0,x1,y1; } Rect;
typedef struct { void *reference,*section; unsigned identifier; void *controls,*caption; } compositor_window_view_t;
static unsigned caption_mask;
static void caption_text_put(void *caption){
 unsigned id=(unsigned)(uintptr_t)caption;assert(id>=1&&id<=4&&!(caption_mask&(1U<<id)));
 caption_mask|=1U<<id;
}
typedef struct {
 bool damage_full_captured,damage_tiles_captured,drag_blit_valid,animations;
 unsigned damage_count,window_count,dragging_identifier;
 Rect damage_rects[16]; compositor_window_view_t windows[4];
} compositor_snapshot_t;
static compositor_snapshot_t g_compositor_snapshot;
static struct { unsigned display_width,display_height; } g_window_server={1920,1080};
static unsigned floor_index,removed_id,expected_count,expected_ids[4],metadata_ids[4],builds,puts,releases;
static unsigned document_puts,document_mask;
static void window_control_document_put(void *document){
 unsigned id=(unsigned)(uintptr_t)document;assert(id>=1&&id<=4&&!(document_mask&(1U<<id)));
 document_mask|=1U<<id;++document_puts;
}
static void window_surface_snapshot_release(compositor_window_view_t *v){assert(v->reference);++releases;}
static unsigned compositor_snapshot_tiles_to_rects(compositor_snapshot_t *s,unsigned n){
 (void)n;s->damage_rects[0]=(Rect){0,0,64,64};return 1;
}
static void compositor_collapse_drag_damage(compositor_snapshot_t *s){(void)s;}
static unsigned compositor_snapshot_occlusion_floor(compositor_snapshot_t *s){(void)s;return floor_index;}
static bool compositor_view_intersects_snapshot_damage(compositor_window_view_t *v,compositor_snapshot_t *s){
 (void)s;return v->identifier!=removed_id;
}
static void object_put(void *p){assert(p);++puts;}
static bool compositor_tile_metadata_build(compositor_snapshot_t *s){
 ++builds;assert(s->window_count==expected_count);
 for(unsigned i=0;i<s->window_count;i++){
  assert(s->windows[i].identifier==expected_ids[i]);metadata_ids[i]=s->windows[i].identifier;
 }
 return true;
}
static bool compositor_build_render_plan(compositor_snapshot_t *s){
 assert(builds==1);for(unsigned i=0;i<s->window_count;i++)assert(metadata_ids[i]==s->windows[i].identifier);
 return true;
}
''' + release + '\n' + function + r'''
static void run(unsigned floor,unsigned remove,unsigned drag,bool animated){
 memset(&g_compositor_snapshot,0,sizeof(g_compositor_snapshot));
 builds=puts=releases=document_puts=document_mask=caption_mask=0;floor_index=floor;removed_id=remove;expected_count=0;
 g_compositor_snapshot.window_count=4;g_compositor_snapshot.damage_count=1;
 g_compositor_snapshot.dragging_identifier=drag;
 g_compositor_snapshot.animations=animated;
 g_compositor_snapshot.damage_rects[0]=(Rect){32,32,64,64};
 for(unsigned i=0;i<4;i++){
  unsigned id=10+i;
  g_compositor_snapshot.windows[i]=(compositor_window_view_t){.reference=(void*)(uintptr_t)(i+1),
   .section=(void*)(uintptr_t)(i+1),.identifier=id,.controls=(void*)(uintptr_t)(i+1),
   .caption=(void*)(uintptr_t)(i+1)};
  if((animated||i>=floor)&&(id!=remove||id==drag))expected_ids[expected_count++]=id;
 }
 compositor_snapshot_plan();assert(builds==1&&puts==4-expected_count&&releases==puts);
 assert(document_puts==puts&&caption_mask==document_mask);
 for(unsigned i=0;i<expected_count;++i){
  compositor_window_view_t *view=g_compositor_snapshot.windows+i;
  assert(view->controls==(void*)(uintptr_t)(view->identifier-9));
  assert(view->caption==view->controls);
  compositor_release_window_snapshot(view);
 }
 assert(document_puts==4&&document_mask==30&&caption_mask==30&&puts==4&&releases==4);
 /* 已压缩的源槽位和已释放的目标再次清理，不能重复释放任何资源。 */
 for(unsigned i=0;i<4;++i){
  compositor_release_window_snapshot(&g_compositor_snapshot.windows[i]);
  assert(!g_compositor_snapshot.windows[i].reference&&!g_compositor_snapshot.windows[i].controls);
  assert(!g_compositor_snapshot.windows[i].caption&&!g_compositor_snapshot.windows[i].section);
 }
 assert(document_puts==4&&puts==4&&releases==4&&caption_mask==30);
}
int main(void){
 run(1,0,13,false); /* Lower covered window removed; shell/cube indices shift. */
 run(0,11,13,false); /* Sparse intersection filter, not just a prefix removal. */
 run(0,0,0,false);run(3,0,0,false);run(0,13,13,false);
 run(3,0,0,true);run(3,11,0,true);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='liteos-tile-index-') as directory:
    base = Path(directory)
    fixture, binary = base / 'test.c', base / 'test'
    fixture.write_text(harness)
    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
               str(fixture), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
    # The previous ordering must fail the same compacted-index contract.
    broken = harness.replace('(void)compositor_tile_metadata_build(snapshot);', '')
    broken = broken.replace('first_window_index =',
                            '(void)compositor_tile_metadata_build(snapshot);\n    first_window_index =', 1)
    fixture.write_text(broken)
    subprocess.run(command, check=True)
    assert subprocess.run([str(binary)], stderr=subprocess.DEVNULL).returncode != 0
print('Tile indices: production planner, prefix/sparse pruning, drag retention, one build, negative control passed')
