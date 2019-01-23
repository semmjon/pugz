#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <cstdlib>

#include <exception>
#include <system_error>

// FIXME: separate assert.h
#include <sstream>
#include <iostream>

#include "common/common.hpp"
#include "common/compatibility.hpp"

namespace gatbl {
namespace sys {
// The oxymoric "noinline inline" means that we want a weak non inlineable function
noreturn_attr noinline_fun inline cold_fun void
throw_syserr(const char fmt[]...) // No variadic template, avoiding to generate too much code
{
    va_list args;
    std::string _what;

    int errcode = 0;
    std::swap(errcode, errno);

    int size = vsnprintf(nullptr, 0, fmt, args);
    if (size < 0) {
        std::terminate();
    }
    _what.resize(static_cast<size_t>(size));
    int size2 = vsnprintf(&_what.front(), _what.size() + 1, fmt, args);

    throw std::system_error(errcode, std::generic_category(), _what);
}

template<typename T, typename Tbounded = make_unsigned_t<T>, typename... Args>
forceinline_fun hot_fun Tbounded
check_ret(T ret, Tbounded min, const char* what, Args&&... args)
{
    if (unlikely(ret < T(min))) {
        throw_syserr(what, std::forward<Args>(args)...);
    }
    return Tbounded(ret);
}

template<typename T, typename... Args>
forceinline_fun hot_fun make_unsigned_t<T>
check_ret(T ret, const char* what, Args&&... args)
{
    return check_ret(ret, make_unsigned_t<T>(0), what, std::forward<Args>(args)...);
}

template<typename T, typename... Args>
forceinline_fun hot_fun T*
check_ptr(T* ret, const char* what, Args&&... args)
{
    if (unlikely(ret == nullptr)) {
        throw_syserr(what, std::forward<Args>(args)...);
    }
    return ret;
}

} // namespace sys
} // namespace gatbl

#endif // EXCEPTIONS_H
