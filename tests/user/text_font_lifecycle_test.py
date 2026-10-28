"""Production font lifecycle with allocation failures and move-only ownership."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'user/runtime/text.c').read_text()
production = source[source.index('typedef struct ui_text_state {'):
                    source.index('uint32_t ui_text_size(void)')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "user/runtime/text.h"
#include "user/runtime/text_metrics.h"
#include <uapi/font.h>
#include <uapi/control_tree.h>
#define BL_SUCCESS 0
#define BL_FILE_READ_NO_FLAGS 0
#define BL_TEXT_ENCODING_UTF8 0
typedef struct {unsigned id;} BLFontCore;
typedef BLFontCore BLFontDataCore;
typedef BLFontCore BLFontFaceCore;
typedef BLFontCore BLImageCore;
typedef BLFontCore BLContextCore;
typedef BLFontCore BLGlyphBufferCore;
typedef struct {float ascent,descent,line_gap;} BLFontMetrics;
typedef struct {struct {double x,y;} advance;} BLTextMetrics;
enum {RUNTIME=1,DATA_INIT,DATA_LOAD,FACE_INIT,FACE_LOAD,FONT_INIT,
      FONT_CREATE,METRICS,GLYPH_INIT,GLYPH_TEXT,TEXT_METRICS};
static unsigned failure,calls,serial,live,runtime,ends;
static const char *expected_path="/EFI/OS/fonts/default.ttf";
static unsigned expected_face;
static bool objects[8192];
static float sizes[8192];
static int result(unsigned stage){++calls;return failure==stage?1:0;}
static void create(BLFontCore *object){assert(!object->id);object->id=++serial;
 assert(serial<8192);objects[serial]=true;++live;}
static int destroy(BLFontCore *object){if(object->id){assert(objects[object->id]);
 objects[object->id]=false;--live;object->id=0;}return 0;}
static int bl_runtime_init(void){int r=result(RUNTIME);if(!r){assert(!runtime);runtime=1;}return r;}
static int bl_runtime_shutdown(void){assert(runtime);runtime=0;return 0;}
static int init(BLFontCore *o,unsigned stage){int r=result(stage);if(!r)create(o);return r;}
static int bl_font_data_init(BLFontDataCore *o){return init(o,DATA_INIT);}
static int bl_font_face_init(BLFontFaceCore *o){return init(o,FACE_INIT);}
static int bl_font_init(BLFontCore *o){return init(o,FONT_INIT);}
static int bl_font_data_create_from_file(BLFontDataCore *o,const char *p,unsigned f){
 assert(objects[o->id]&&!strcmp(p,expected_path)&&!f);return result(DATA_LOAD);}
static int bl_font_face_create_from_data(BLFontFaceCore *o,BLFontDataCore *d,unsigned i){
 assert(objects[o->id]&&objects[d->id]&&i==expected_face);return result(FACE_LOAD);}
static int bl_font_create_from_face(BLFontCore *o,BLFontFaceCore *f,float s){
 assert(objects[o->id]&&objects[f->id]);int r=result(FONT_CREATE);if(!r)sizes[o->id]=s;return r;}
static int bl_font_get_metrics(BLFontCore *o,BLFontMetrics *m){
 assert(objects[o->id]);int r=result(METRICS);m->ascent=sizes[o->id]*.8f;
 m->descent=sizes[o->id]*.3f;m->line_gap=0;return r;}



static int bl_font_init_move(BLFontCore *o,BLFontCore *from){
 assert(!o->id&&objects[from->id]);*o=*from;from->id=0;return 0;}
static int bl_font_destroy(BLFontCore *o){return destroy(o);}
static int bl_font_data_destroy(BLFontDataCore *o){return destroy(o);}
static int bl_font_face_destroy(BLFontFaceCore *o){return destroy(o);}

static int bl_context_end(BLContextCore *o){assert(objects[o->id]);++ends;return 0;}
static int bl_context_destroy(BLContextCore *o){return destroy(o);}
static int bl_image_destroy(BLImageCore *o){return destroy(o);}
''' + production + r'''
static uint32_t pixel;
static void context(void){assert(!g_text.context_ready);create(&g_text.image);
 create(&g_text.context);g_text.context_ready=true;g_text.pixels=&pixel;
 g_text.width=100;g_text.height=80;g_text.stride=120;}
static void fresh(void){failure=0;ui_text_shutdown();assert(!live&&!runtime);
 memset(&g_text,0,sizeof(g_text));}
int main(void){
 assert(ui_text_init(16));assert(!strcmp(g_font_path,OS_FONT_DEFAULT_PATH));
 assert(ui_text_adjust(1));fresh();
 assert(text_length(NULL)==0&&text_length("font")==4);
 for(unsigned stage=RUNTIME;stage<=METRICS;++stage){
  fresh();failure=stage;assert(!ui_text_init(16));
  assert(!live&&!runtime&&!g_text.font_ready&&!g_text.size);
  failure=0;assert(ui_text_init(16));assert(live==3&&runtime);
 }
 fresh();assert(ui_text_init(16));context();
 for(unsigned stage=FONT_INIT;stage<=METRICS;++stage){
  ui_text_state_t before=g_text;unsigned prior=ends;
  failure=stage;assert(!ui_text_init(32));
  assert(!memcmp(&before,&g_text,sizeof(g_text))&&ends==prior&&live==5&&runtime);
  assert(objects[g_text.font.id]&&objects[g_text.context.id]);
  assert(!ui_text_adjust(1));assert(!memcmp(&before,&g_text,sizeof(g_text)));
 }
 failure=0;unsigned prior=calls;assert(ui_text_init(16));assert(calls==prior);
 assert(!ui_text_init(13)&&!ui_text_init(51));assert(calls==prior);
 assert(!ui_text_adjust(0));assert(calls==prior);
 unsigned old=g_text.font.id,old_context=g_text.context.id,ended=ends;
 assert(ui_text_adjust(1));assert(g_text.size==20&&g_text.advance==10);
 assert(g_text.line_height==22&&g_text.baseline==16);
 assert(!objects[old]&&!objects[old_context]&&ends==ended+1&&live==3);
 assert(!g_text.context_ready&&!g_text.pixels&&!g_text.width&&!g_text.height);
 ui_text_state_t cached=g_text;failure=METRICS;
 assert(!ui_text_init(24)&&!memcmp(&cached,&g_text,sizeof(g_text)));
 failure=0;assert(ui_text_init(24));assert(live==3);
 fresh();assert(ui_text_init(16));
 for(unsigned round=0;round<30;++round){
  for(unsigned size=14;size<=50;++size){
   context();assert(ui_text_init(size));assert(g_text.size==size);
   if(g_text.context_ready)close_context();
   assert(live==3&&g_text.line_height>=size+2&&g_text.baseline<g_text.line_height);
   assert(g_text.advance>0);
  }
 }
 assert(!ui_text_adjust(1));assert(ui_text_adjust(-1)&&g_text.size==46);
 while(ui_text_adjust(-1)){}assert(g_text.size==14);
 assert(!ui_text_adjust(-1));assert(ui_text_adjust(1)&&g_text.size==18);
 fresh();unsigned prior_ends=ends;ui_text_shutdown();assert(!live&&!runtime&&ends==prior_ends);
 puts("Font lifecycle: initial failures, transactional zoom rollback, fresh metrics, ownership, bounds and 1110 repeated switches passed");
}
'''
with tempfile.TemporaryDirectory(prefix='system-font-lifecycle-') as directory:
    path = Path(directory)
    c_file = path / 'font.c'
    c_file.write_text(program)
    binary = path / 'font'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I' + str(ROOT), '-I' + str(ROOT/'include'), str(c_file), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
