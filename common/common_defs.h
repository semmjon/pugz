/*
 * common_defs.h
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef COMMON_COMMON_DEFS_H
#define COMMON_COMMON_DEFS_H

/* ========================================================================== */
/*                              Type definitions                              */
/* ========================================================================== */

#include <cstdlib>
#include <cstddef> /* size_t */

#ifdef __cpp_lib_byte
using byte = std::byte;
#else
enum class byte : unsigned char
{
};
#endif

/*
 * Word type of the target architecture.  Use 'size_t' instead of 'unsigned
 * long' to account for platforms such as Windows that use 32-bit 'unsigned
 * long' on 64-bit architectures.
 */
typedef size_t machine_word_t;

#if defined(PRINT_DEBUG) && PRINT_DEBUG
#    undef PRINT_DEBUG
#    define PRINT_DEBUG(...)                                                                                                                                   \
        {                                                                                                                                                      \
            fprintf(stderr, __VA_ARGS__);                                                                                                                      \
            fflush(stderr);                                                                                                                                    \
        }
#else
#    undef PRINT_DEBUG
#    define PRINT_DEBUG(...)                                                                                                                                   \
        {}
#endif

#if defined(PRINT_DEBUG_DECODING) && PRINT_DEBUG_DECODING
#    undef PRINT_DEBUG_DECODING
#    define PRINT_DEBUG_DECODING(x)                                                                                                                            \
        {                                                                                                                                                      \
            fprintf(stderr, __VA_ARGS__);                                                                                                                      \
            fflush(stderr);                                                                                                                                    \
        }
#else
#    undef PRINT_DEBUG_DECODING
#    define PRINT_DEBUG_DECODING(x)                                                                                                                            \
        {}
#endif

#endif
