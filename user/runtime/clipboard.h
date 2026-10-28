#pragma once
#include <stdbool.h>
#include <stdlib.h>
#include <uapi/clipboard.h>
#include <uapi/syscall.h>
typedef int64_t (*ui_clipboard_call_t)(uint64_t,uint64_t);
static inline bool ui_clipboard_copy(ui_clipboard_call_t call,const char *text,size_t size) {
    os_clipboard_text_t r={.hdr={sizeof(r),OS_SYSCALL_ABI_VERSION},.data=(uintptr_t)text,.size=size};
    return call && call(OS_SYS_CLIPBOARD_SET,(uintptr_t)&r)==0;
}
/* 查询与读取间允许其他应用更新；容量不足时重试，不返回截断的 UTF-8。 */
static inline char *ui_clipboard_paste(ui_clipboard_call_t call,size_t limit,size_t *size) {
    if(!call || !size)return 0;
    for(unsigned attempt=0;attempt<3U;++attempt) {
        os_clipboard_text_t r={.hdr={sizeof(r),OS_SYSCALL_ABI_VERSION}};
        if(call(OS_SYS_CLIPBOARD_GET,(uintptr_t)&r) || !r.size || r.size>limit ||
            r.size>OS_CLIPBOARD_MAX_BYTES)return 0;
        size_t capacity=(size_t)r.size;
        char *text=malloc(capacity+1U);if(!text)return 0;
        r.data=(uintptr_t)text;
        int64_t result=call(OS_SYS_CLIPBOARD_GET,(uintptr_t)&r);
        if(!result && !r.size){free(text);return 0;}
        if(!result && r.size<=capacity){text[r.size]=0;*size=(size_t)r.size;return text;}
        free(text);if(result!=-28)return 0;
    }
    return 0;
}
