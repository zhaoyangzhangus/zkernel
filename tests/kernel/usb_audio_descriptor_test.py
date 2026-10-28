"""用主板真实 UAC2 描述符和截断/备用接口用例验证播放路由选择。"""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
fixture=(root/'tests/fixtures/usb-audio-0db0-cc78.bin').read_bytes()
earpods=(root/'tests/fixtures/usb-audio-05ac-110b.bin').read_bytes()
source=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "kernel/drivers/usb/xhci/audio_descriptor.h"
static const uint8_t fixture[]={'''+','.join(map(str,fixture))+r'''};
static const uint8_t earpods[]={'''+','.join(map(str,earpods))+r'''};
int main(void){
 xhci_audio_descriptor_t out;
 assert(xhci_select_audio_descriptor(earpods+18,sizeof(earpods)-18,&out));
 assert(out.interface_number==1 && out.alternate==2 && out.clock==9 && out.endpoint==2);
 assert(out.mute_unit==2 && out.volume_unit==2 && out.volume_channels==6);
 assert(out.interval==1 && out.max_packet==192 && out.bits==16);
 assert(xhci_select_audio_descriptor(fixture+18,sizeof(fixture)-18,&out));
 assert(out.interface_number==4 && out.alternate==1 && out.protocol==0x20);
 assert(out.control_interface==0 && out.clock==5 && out.endpoint==5);
 assert(out.channels==2 && out.bits==16 && out.subslot==2 && out.rate==48000);
 assert(out.interval==1 && out.max_packet>=24);
 for(unsigned n=0;n<sizeof(fixture);++n){
   xhci_audio_descriptor_t sentinel;memset(&sentinel,0xA5,sizeof(sentinel));out=sentinel;
   if(!xhci_select_audio_descriptor(fixture,n,&out))assert(!memcmp(&out,&sentinel,sizeof(out)));
 }
 uint8_t uac1[]={9,2,0,0,1,1,0,0x80,50,
    9,4,2,1,1,1,2,0,0,7,0x24,1,1,1,1,0,
    14,0x24,2,1,2,2,16,2,0x44,0xac,0,0x80,0xbb,0,
    7,5,1,9,0xc0,0,1};
 assert(xhci_select_audio_descriptor(uac1,sizeof(uac1),&out));
 assert(out.protocol==0 && out.endpoint==1 && out.rate==48000);
 uac1[sizeof(uac1)-5]=0x81;assert(!xhci_select_audio_descriptor(uac1,sizeof(uac1),&out));
 uac1[sizeof(uac1)-5]=1;uac1[36]=0;assert(!xhci_select_audio_descriptor(uac1,sizeof(uac1),&out));
 puts("USB Audio descriptor: physical UAC2, UAC1 rates, capture rejection and all truncations passed");
}
'''
with tempfile.TemporaryDirectory() as d:
 exe=str(Path(d)/'test')
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(root),'-x','c','-','-o',exe],input=source,text=True,check=True)
 subprocess.run([exe],check=True)
