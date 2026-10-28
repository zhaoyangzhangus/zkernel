"""验证真实 HDA 格式编码、描述符顺序与数字输出连接选择。"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/drivers/audio/hda/core.c').read_text()
def function(name):
 start=s.rfind('\n',0,s.index(name+'('))+1
 brace=s.index('{',start);end=brace+1;depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]+'\n'
code=s[:s.index('static hda_controller_t g_hda;')]+r'''
#include <assert.h>
#include <stdio.h>
static bool connected=true,dp=false;
static uint8_t frame[14];
static unsigned writes,slots,dips;
static bool hda_codec_command(hda_controller_t *c,uint8_t cad,uint8_t node,uint16_t verb,uint16_t value,uint32_t *out){
 (void)c;assert(cad==0);*out=0;
 if(verb==HDA_VERB_PARAMETERS){
  if(value==HDA_PARAM_NODE_COUNT){assert(node==1);*out=(2U<<16)|4U;}
  else if(value==HDA_PARAM_WIDGET_CAP){*out=node<4?0x221U:0x400381U;}
  else if(value==0x0C){*out=0x94U;}
  else if(value==0x0E){*out=1U;}
  else assert(0);
 }else if(verb==0xF09){*out=connected&&node==5?1U<<31:0;}
 else if(verb==0xF02){assert(!value);*out=node==4?2:3;}
 else if(verb==0xF2F){*out=dp?((1U<<31)|4U):0;}
 else {
  ++writes;
  if(verb==0x734){assert(value==0 || value==0x11);++slots;}
  if(verb==0x731){frame[dips%14]=value;++dips;}
 }
 return true;
}
'''+function('hda_codec_set')+function('hda_codec_node_start')+function('hda_codec_node_count')+function('hda_discover_output_route')+function('hda_program_output_route')+function('hda_decode_gcap')+function('hda_stream_offset')+function('hda_audio_format_value')+r'''
int main(void){
 const uint32_t rate[]={8000,11025,16000,22050,24000,32000,44100,48000,88200,96000,176400,192000};
 const uint16_t encoded[]={0x500,0x4300,0x200,0x4100,0x100,0xa00,0x4000,0,0x4800,0x800,0x5800,0x1800};
 for(unsigned r=0;r<12;r++)for(unsigned ch=1;ch<=16;ch++)for(unsigned bits=1;bits<=3;bits++){
  audio_format_t f={.sample_rate=rate[r],.channels=ch,.sample_format=bits};
  uint16_t e=hda_audio_format_value(&f);
  assert(e==(encoded[r]|((bits==1?1:bits==2?3:4)<<4)|(ch-1)));
  unsigned base=e&(1U<<14)?44100:48000;
  assert(base*(((e>>11)&7)+1)/(((e>>8)&7)+1)==rate[r]);
 }
 audio_format_t f={.sample_rate=12345,.channels=2,.sample_format=1};assert(!hda_audio_format_value(&f));
 hda_controller_t c={.input_streams=4,.output_streams=4,.bidirectional_streams=2,.codec_function_node=1};
 assert(hda_stream_offset(&c,true,3)==0xe0 && hda_stream_offset(&c,false,0)==0x100);
 assert(hda_stream_offset(&c,false,3)==0x160 && hda_stream_offset(&c,false,4)==UINT32_MAX);
 hda_controller_t decoded={0};
 for(unsigned oss=0;oss<16;oss++)for(unsigned iss=0;iss<16;iss++)for(unsigned bss=0;bss<32;bss++){
   hda_decode_gcap(&decoded,(oss<<12)|(iss<<8)|(bss<<3)|7);
   assert(decoded.output_streams==oss && decoded.input_streams==iss && decoded.bidirectional_streams==bss);
 }
 assert(hda_discover_output_route(&c));assert(c.codec_output_pin==5 && c.codec_output_node==3);
 assert(hda_program_output_route(&c,0x11));assert(slots==2 && dips==14 && writes>14);
 dp=true;c.codec_vendor_id=0x10de00af;
 assert(hda_program_output_route(&c,0x11));
 assert(frame[0]==0x84 && frame[1]==0x1b && frame[2]==0x44 && frame[4]==1);
 unsigned checksum=0;for(unsigned i=0;i<14;i++)checksum+=frame[i];assert(!(checksum&255));
 c.codec_vendor_id=0x1002aa01;assert(hda_program_output_route(&c,0x11));
 assert(frame[3]==1 && frame[4]==0);
 connected=false;assert(hda_discover_output_route(&c));assert(c.codec_output_pin==4 && c.codec_output_node==2);
 assert(!hda_program_output_route(&c,0x17));
 puts("HDA: 576 real format encodings, stream offsets, connected pin routing and stereo digital verbs passed");
}
'''
with tempfile.TemporaryDirectory() as d:
 exe=str(Path(d)/'test')
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root/'include'),'-x','c','-','-o',exe],input=code,text=True,check=True)
 subprocess.run([exe],check=True)
