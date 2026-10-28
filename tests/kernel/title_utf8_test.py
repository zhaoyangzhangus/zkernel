"""中文标题使用真实字体解析和光栅化，并验证 UTF-8 截断与预热失败清理。"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
io = (root/'kernel/fs/vfs/file_io.c').read_text()
file_loader = io[io.index('kstatus_t vfs_load_file_kernel('):io.index('/* 整次写入')]
source = (root / 'kernel/graphics/ascii_font.c').read_text()
declarations = source[source.index('static ascii_font_parser_t g_ui_font_parser;'):source.index('static ascii_font_point_t font_midpoint')]
raster = source[source.index('static ascii_font_point_t font_midpoint'):source.index('static bool font_rasterize_glyph(')]
loader = source[source.index('bool ui_font_load('):source.index('static void font_user_cache_lock(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "ascii_font_internal.h"
#include <uapi/utf8.h>
#include "user/runtime/caption.h"
#include <kernel/caption_text.h>
#define ASCII_FONT_SUPERSAMPLE 4U
#define TITLE_FONT_HEIGHT 20U
static const uint32_t title_codes[]=U"设置终端文件管理器网络任务图片查看主文件夹打开删除属性重命名复制剪切粘贴新建";
#define ASCII_FONT_MAX_DIMENSION 96U
static ascii_font_parser_t g_ascii_font_parser;
#define K_OK 0
#define K_EINVAL -22
#define K_ENOMEM -12
#define K_EIO -5
#define VFS_OPEN_READ 1
typedef int kstatus_t;
typedef struct {uint64_t size;} vnode_t;
typedef struct {vnode_t *vnode;} file_t;
static vnode_t vnode;
static file_t file={&vnode};
static void vfs_vnode_lock(vnode_t *v){assert(v==&vnode);}
static void vfs_vnode_unlock(vnode_t *v){assert(v==&vnode);}
static uint8_t *data;
static unsigned allocations,frees,opens,closes;
static bool oom,short_read;
static void *kmalloc(size_t size,unsigned flags){assert(!flags);if(oom)return NULL;++allocations;return malloc(size);}
static void kfree(void *p){if(p){++frees;free(p);}}
static int vfs_open_kernel(const char *p,unsigned f,unsigned m,file_t **out){
 assert(!strcmp(p,"/EFI/OS/fonts/cjk.ttf")&&f==1&&!m);++opens;*out=&file;return K_OK;
}
static void vfs_close(file_t *p){assert(p==&file);++closes;}
static int vfs_read_kernel(file_t *p,void *buffer,size_t size,uint64_t *read){
 assert(p==&file&&size==vnode.size);memcpy(buffer,data,size);*read=size-(short_read?1:0);return K_OK;
}
''' + file_loader + declarations + raster + loader + (root/'kernel/graphics/caption_text.c').read_text().split('caption_text_t *caption_text_create',1)[1].join(['caption_text_t *caption_text_create','']) + r'''
int main(int argc,char **argv){
 assert(argc==3);FILE *input=fopen(argv[1],"rb");assert(input);
 fseek(input,0,SEEK_END);vnode.size=ftell(input);rewind(input);
 data=malloc(vnode.size);assert(data&&fread(data,1,vnode.size,input)==vnode.size);fclose(input);
 /* 独立 FreeType 轮廓对照，不能只用同一个解析器证明自身正确。 */
 FT_Library library;FT_Face face;assert(!FT_Init_FreeType(&library));
 assert(!FT_New_Memory_Face(library,data,(FT_Long)vnode.size,0,&face));
 ascii_font_parser_t parser;assert(ascii_font_parser_init(&parser,data,vnode.size));
 for(size_t i=0;title_codes[i];++i){
  uint16_t glyph=ascii_font_parser_glyph_for_code(&parser,title_codes[i]);
  assert(glyph==FT_Get_Char_Index(face,title_codes[i]));
  assert(!FT_Load_Glyph(face,glyph,FT_LOAD_NO_SCALE|FT_LOAD_NO_HINTING|FT_LOAD_NO_BITMAP));
  ascii_font_point_t points[ASCII_FONT_MAX_POINTS];uint16_t ends[ASCII_FONT_MAX_CONTOURS];
  uint32_t count,contours;assert(ascii_font_parser_decode_simple_glyph(&parser,glyph,points,ends,&count,&contours));
  assert(count==(uint32_t)face->glyph->outline.n_points&&contours==(uint32_t)face->glyph->outline.n_contours);
  for(uint32_t j=0;j<count;++j){
   assert(labs(points[j].x-face->glyph->outline.points[j].x)<=1);
   assert(labs(points[j].y-face->glyph->outline.points[j].y)<=1);
   assert(points[j].on_curve==!!(face->glyph->outline.tags[j]&1));
  }
  for(uint32_t j=0;j<contours;++j)assert(ends[j]==face->glyph->outline.contours[j]);
 }
 FT_Done_Face(face);FT_Done_FreeType(library);
 oom=true;assert(!ui_font_load());oom=false;
 short_read=true;assert(!ui_font_load());short_read=false;
 uint8_t saved=data[0];data[0]=0xff;assert(!ui_font_load());data[0]=saved;
 assert(ui_font_load());unsigned count=0;
 for(size_t i=0;title_codes[i];++i){
  uint8_t glyph[400];bool copied=ui_font_copy_glyph(title_codes[i],20,20,glyph,sizeof(glyph));
  if(!copied)fprintf(stderr,"shared glyph failure U+%04X\n",title_codes[i]);assert(copied);
  unsigned coverage=0;for(unsigned j=0;j<TITLE_FONT_HEIGHT*TITLE_FONT_HEIGHT;++j)coverage+=glyph[j];
  assert(coverage>0&&coverage<TITLE_FONT_HEIGHT*TITLE_FONT_HEIGHT*255);++count;
 }
 unsigned old=opens;assert(ui_font_load()&&opens==old);
 assert(allocations==frees+1&&opens==closes);assert(!ui_font_copy_glyph(0x10FFFF,20,20,(uint8_t[400]){0},400));
 input=fopen(argv[2],"rb");assert(input);fseek(input,0,SEEK_END);
 size_t latin_size=ftell(input);rewind(input);uint8_t *latin=malloc(latin_size);assert(latin);
 assert(fread(latin,1,latin_size,input)==latin_size);fclose(input);
 assert(ascii_font_parser_init(&g_ascii_font_parser,latin,latin_size));
 /* 公共字形接口覆盖标题预热集合以外的菜单文字；尺寸间互不污染。 */
 const uint32_t controls[]=U"主文件夹打开删除属性重命名复制剪切粘贴新建…é";
 uint8_t output[2+96*96],reference[96*96],saved_title[TITLE_FONT_HEIGHT*TITLE_FONT_HEIGHT];
 assert(ui_font_copy_glyph(0x6587,20,20,saved_title,sizeof(saved_title)));
 unsigned before_allocations=allocations,before_opens=opens;
 for(size_t i=0;controls[i];++i)for(unsigned size=16;size<=48;size+=8){
  memset(output,0x5a,sizeof(output));
  bool copied=ui_font_copy_glyph(controls[i],size,size,output+1,size*size);
  if(!copied)fprintf(stderr,"control glyph failed U+%04X size=%u\n",controls[i],size);
  assert(copied);
  assert(output[0]==0x5a&&output[1+size*size]==0x5a);
  const ascii_font_parser_t *face=&g_ascii_font_parser;
  uint16_t glyph=ascii_font_parser_glyph_for_code(face,controls[i]);
  if(!glyph){face=&parser;glyph=ascii_font_parser_glyph_for_code(face,controls[i]);}assert(glyph);
  assert(font_rasterize_from_parser(face,glyph,reference,size,size));
  assert(!memcmp(output+1,reference,size*size));
 }
 assert(ui_font_copy_glyph(0x6587,20,20,output+1,400));
 assert(!memcmp(output+1,saved_title,400));
 memset(output,0x5a,sizeof(output));
 assert(!ui_font_copy_glyph(0x6587,0,20,output+1,400));
 assert(!ui_font_copy_glyph(0x6587,97,20,output+1,sizeof(output)-2));
 assert(!ui_font_copy_glyph(0x6587,20,20,output+1,399));
 assert(!ui_font_copy_glyph(0x6587,20,20,NULL,400));
 assert(!ui_font_copy_glyph(0xd800,20,20,output+1,400));
 assert(!ui_font_copy_glyph(0x110000,20,20,output+1,400));
 assert(!ui_font_copy_glyph(0x10ffff,20,20,output+1,400));
 for(size_t i=0;i<sizeof(output);++i)assert(output[i]==0x5a);
 assert(before_allocations==allocations&&before_opens==opens);
 assert(ui_font_copy_glyph('A',20,20,output+1,400));
 /* 标题快照与正文调用同一服务，任意中文和字号均不再依赖预热白名单。 */
 for(unsigned h=14;h<=50;h+=4) {
  caption_text_t *caption=caption_text_create("设置 - 测试é",h);assert(caption);
  assert(caption->height==h&&caption->width==5U*h+3U*(h/2U));
  caption_text_get(caption);assert(atomic_load(&caption->references)==2);
  caption_text_put(caption);assert(atomic_load(&caption->references)==1);
  uint8_t expected[96*96];assert(ui_font_copy_glyph(0x8bbe,h,h,expected,sizeof(expected)));
  for(unsigned y=0;y<h;++y)assert(!memcmp(caption->pixels+y*caption->width,expected+y*h,h));
  caption_text_put(caption);
 }
 caption_text_t *empty=caption_text_create("",16);assert(empty&&empty->width==0);caption_text_put(empty);
 oom=true;assert(!caption_text_create("设置",16));oom=false;
 assert(!caption_text_create("设置",0)&&!caption_text_create("设置",97));
 assert(!caption_text_create("12345678901234567890123456789012",16));
 char title[32];user_caption_format_app_title(title,"记事本","/notes.txt",true);
 assert(!strcmp(title,"* 记事本 - /notes.txt"));
 const char *path="/中文中文中文中文中文中文中文中文";
 for(unsigned prefix=0;prefix<2;++prefix){
  user_caption_format_title(title,path,prefix);
  size_t offset=0;while(title[offset]){size_t used;
   assert(os_utf8_next(title+offset,sizeof(title)-offset,&used)!=0xFFFD);
   assert(used);offset+=used;assert(offset<32);
  }
 }
 const char *invalid[]={"\x80","\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xe4\xb8"};
 for(unsigned i=0;i<5;++i){size_t used;assert(os_utf8_next(invalid[i],strlen(invalid[i]),&used)==0xFFFD&&used==1);}
 for(size_t n=0;n<3;++n){size_t used;assert(os_utf8_next("中",n,&used)==(n?0xFFFD:0));assert(used==(n?1:0));}
 free(data);data=NULL;
 assert(ui_font_copy_glyph(0x6587,20,20,output+1,400));
 assert(!memcmp(output+1,saved_title,400));
 kfree((void *)g_ui_font_parser.file);g_ui_font_parser=(ascii_font_parser_t){0};
 assert(allocations==frees);
 free(latin);g_ascii_font_parser=(ascii_font_parser_t){0};
 printf("Chinese titles: %u real CJK glyphs, cache reuse, OOM/short read/corruption cleanup, UTF-8 limits; scalable Unicode controls, bounds, independent cache lifetime and allocation-free copies PASS\n",count);
}
'''
with tempfile.TemporaryDirectory(prefix='title-utf8-') as directory:
    binary = str(Path(directory) / 'test')
    freetype = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'freetype2'], text=True).split()
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    '-I'+str(root), '-I'+str(root/'include'),
                    '-I'+str(root/'kernel/graphics'), '-x', 'c', '-',
                    str(root/'kernel/graphics/ascii_font_parser.c'), '-o', binary, *freetype],
                   input=program, text=True, check=True)
    subprocess.run([binary, str(root/'assets/fonts/title-cjk.ttf'),str(root/'assets/fonts/default.ttf')], check=True)
