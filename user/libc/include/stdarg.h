#pragma once

/* GCC 内置函数是独立运行目标唯一可用的可变参数机制；在此公开这些函数可避免依赖宿主头文件。 */
typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(destination, source) __builtin_va_copy(destination, source)
