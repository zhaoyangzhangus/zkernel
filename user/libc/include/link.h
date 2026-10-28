#pragma once

#include <elf.h>

/* System ELF 声明位于 elf.h；此头文件提供 POSIX 代码所需的 glibc 兼容包含路径和
 * ElfW() 宏。 */
#ifndef ElfW
#define ElfW(type) Elf64_##type
#endif
