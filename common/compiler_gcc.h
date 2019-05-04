/*
 * compiler_gcc.h - definitions for the GNU C Compiler.  This also handles clang
 * and the Intel C Compiler (icc).
 *
 * TODO: icc is not well tested, so some things are currently disabled even
 * though they maybe can be enabled on some icc versions.
 */

#if !defined(__clang__) && !defined(__INTEL_COMPILER)
#    define GCC_PREREQ(major, minor) (__GNUC__ > (major) || (__GNUC__ == (major) && __GNUC_MINOR__ >= (minor)))
#else
#    define GCC_PREREQ(major, minor) 0
#endif

/* Note: only check the clang version when absolutely necessary!
 * "Vendors" such as Apple can use different version numbers. */
#ifdef __clang__
#    ifdef __apple_build_version__
#        define CLANG_PREREQ(major, minor, apple_version) (__apple_build_version__ >= (apple_version))
#    else
#        define CLANG_PREREQ(major, minor, apple_version) (__clang_major__ > (major) || (__clang_major__ == (major) && __clang_minor__ >= (minor)))
#    endif
#else
#    define CLANG_PREREQ(major, minor, apple_version) 0
#endif

#ifndef __has_attribute
#    define __has_attribute(attribute) 0
#endif
#ifndef __has_feature
#    define __has_feature(feature) 0
#endif
#ifndef __has_builtin
#    define __has_builtin(builtin) 0
#endif

#ifdef _WIN32
#    define LIBEXPORT __declspec(dllexport)
#else
#    define LIBEXPORT __attribute__((visibility("default")))
#endif

#define inline inline
#define forceinline inline __attribute__((always_inline))
#define restrict __restrict__
#define likely(expr) __builtin_expect(!!(expr), 1)
#define unlikely(expr) __builtin_expect(!!(expr), 0)
#define prefetchr(addr) __builtin_prefetch((addr), 0)
#define prefetchw(addr) __builtin_prefetch((addr), 1)
#define _aligned_attribute(n) __attribute__((aligned(n)))
