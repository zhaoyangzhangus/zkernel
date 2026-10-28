#pragma once

/* ELF libc 与 Chromium/libc++ 共用 Linux x86-64 wchar ABI。
 * PE/UEFI 代码仍使用独立的 16 位宽字符约定。 */
#if !defined(__ASSEMBLER__) && __SIZEOF_WCHAR_T__ != 4
#error "System ELF user ABI requires 32-bit wchar_t"
#endif
