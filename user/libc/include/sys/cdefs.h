#pragma once

/*
 * 可移植 POSIX 代码使用的精简 libc 中立兼容接口。
 * System 并非 glibc，因此若包含宿主 sysroot 中的 cdefs.h，就会引入 glibc 特性机制，并使目标编译
 * 依赖宿主宏。
 */

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

#ifndef __GNUC_PREREQ
#if defined(__GNUC__) && defined(__GNUC_MINOR__)
#define __GNUC_PREREQ(major, minor) \
    ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((major) << 16) + (minor))
#else
#define __GNUC_PREREQ(major, minor) 0
#endif
#endif

#ifndef __glibc_clang_prereq
#define __glibc_clang_prereq(major, minor) 0
#endif

#ifndef __THROW
#if defined(_NOEXCEPT)
#define __THROW _NOEXCEPT
#elif defined(__cplusplus)
#define __THROW noexcept
#else
#define __THROW
#endif
#endif
#ifndef __THROWNL
#define __THROWNL __THROW
#endif
#ifndef __NTH
#define __NTH(function) function
#endif
#ifndef __NTHNL
#define __NTHNL(function) function
#endif

#ifndef __LEAF
#define __LEAF
#endif
#ifndef __LEAF_ATTR
#define __LEAF_ATTR
#endif
#ifndef __wur
#define __wur
#endif
#ifndef __attribute_malloc__
#define __attribute_malloc__
#endif
#ifndef __attribute_warn_unused_result__
#define __attribute_warn_unused_result__
#endif
#ifndef __attribute_deprecated__
#define __attribute_deprecated__
#endif
#ifndef __attribute_alloc_size__
#define __attribute_alloc_size__(parameters)
#endif
#ifndef __attribute_alloc_align__
#define __attribute_alloc_align__(parameter)
#endif
#ifndef __attribute_format_arg__
#define __attribute_format_arg__(parameter) __attribute__((__format_arg__(parameter)))
#endif
#ifndef __nonnull
#define __nonnull(parameters)
#endif
#ifndef __attr_access
#define __attr_access(parameters)
#endif
