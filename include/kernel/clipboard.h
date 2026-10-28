#pragma once
#include <kernel/base.h>
/* 发布不可变文本快照，读取者的引用不受后续复制操作影响。 */
kstatus_t clipboard_set_text(const char *text,size_t size);
