#include <arch/x86_64/uaccess.h>
#include <kernel/clipboard.h>
#include <kernel/kmem.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <uapi/clipboard.h>
#include "internal.h"

typedef struct clipboard_text {
    atomic_uint references;
    size_t size;
    char text[];
} clipboard_text_t;
static spinlock_t g_clipboard_lock;
static clipboard_text_t *g_clipboard;

static void clipboard_put(clipboard_text_t *text) {
    if(text && atomic_fetch_sub_explicit(&text->references,1U,memory_order_acq_rel)==1U)
        kfree(text);
}
static void clipboard_publish(clipboard_text_t *text) {
    sched_preempt_disable();spinlock_lock(&g_clipboard_lock);
    clipboard_text_t *previous=g_clipboard;g_clipboard=text;
    spinlock_unlock(&g_clipboard_lock);sched_preempt_enable();
    clipboard_put(previous);
}
/* 空剪贴板直接发布空指针，不分配空快照；清空在内存不足时仍可完成。 */
static clipboard_text_t *clipboard_allocate(size_t size) {
    if(!size)return 0;
    clipboard_text_t *text=kmalloc(sizeof(*text)+size,0);
    if(text){atomic_init(&text->references,1U);text->size=size;}
    return text;
}
kstatus_t clipboard_set_text(const char *text,size_t size) {
    if(size>OS_CLIPBOARD_MAX_BYTES || (size && !text))return K_EINVAL;
    clipboard_text_t *next=clipboard_allocate(size);
    if(size && !next)return K_ENOMEM;
    if(size)memcpy(next->text,text,size);
    clipboard_publish(next);return K_OK;
}
int64_t syscall_clipboard_set(uint64_t pointer,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;
    os_clipboard_text_t request;
    kstatus_t status=copy_from_user(&request,(const void __user *)(uintptr_t)pointer,sizeof(request));
    if(status)return status;
    if(!versioned_header_valid(&request.hdr,sizeof(request)) || request.hdr.flags ||
        request.size>OS_CLIPBOARD_MAX_BYTES || (request.size && !request.data))return K_EINVAL;
    clipboard_text_t *next=clipboard_allocate((size_t)request.size);
    if(request.size && !next)return K_ENOMEM;
    if(request.size)status=copy_from_user(next->text,(const void __user *)(uintptr_t)request.data,next->size);
    if(status){clipboard_put(next);return status;}
    clipboard_publish(next);return K_OK;
}
int64_t syscall_clipboard_get(uint64_t pointer,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;
    os_clipboard_text_t request;
    kstatus_t status=copy_from_user(&request,(const void __user *)(uintptr_t)pointer,sizeof(request));
    if(status)return status;
    if(!versioned_header_valid(&request.hdr,sizeof(request)) || request.hdr.flags ||
        request.size>OS_CLIPBOARD_MAX_BYTES || (request.size && !request.data))return K_EINVAL;
    size_t capacity=(size_t)request.size;
    /* 只在短锁内取得引用；分配、用户内存访问和释放全部在锁外。 */
    sched_preempt_disable();spinlock_lock(&g_clipboard_lock);
    clipboard_text_t *text=g_clipboard;
    if(text)atomic_fetch_add_explicit(&text->references,1U,memory_order_relaxed);
    spinlock_unlock(&g_clipboard_lock);sched_preempt_enable();
    request.size=text?text->size:0U;
    if(capacity && capacity<request.size)status=K_ENOSPC;
    else if(capacity && request.size)
        status=copy_to_user((void __user *)(uintptr_t)request.data,text->text,(size_t)request.size);
    clipboard_put(text);
    kstatus_t output=copy_to_user((void __user *)(uintptr_t)pointer,&request,sizeof(request));
    return output?output:status;
}
