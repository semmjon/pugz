#ifndef COMPATIBILITY_HPP
#define COMPATIBILITY_HPP

#if __cplusplus < 201103L
#    error Minimum requirement: C++11
#endif

#include <iterator>
#include <type_traits>
#include <memory>

namespace gatbl {

using std::begin;
using std::end;

using std::advance;
using std::distance;

#if __cplusplus >= 201402L
using std::cbegin;
using std::cend;
using std::crbegin;
using std::crend;
using std::rbegin;
using std::rend;

using std::make_unique;

// Type alias for *::type
using std::conditional_t;
using std::enable_if_t;
using std::make_unsigned_t;
using std::remove_const_t;
using std::remove_reference_t;

#    define CPP14_CONSTEXPR constexpr

#else
template<typename C>
inline constexpr auto
cbegin(const C& c) -> decltype(c.begin())
{
    return c.begin();
}
template<typename C>
inline constexpr auto
cend(const C& c) -> decltype(c.end())
{
    return c.end();
}
template<typename C>
inline constexpr auto
rbegin(C& c) -> decltype(c.rbegin())
{
    return c.rbegin();
}
template<typename C>
inline constexpr auto
rbegin(const C& c) -> decltype(c.rbegin())
{
    return c.rbegin();
}
template<typename C>
inline constexpr auto
rend(C& c) -> decltype(c.rend())
{
    return c.rend();
}
template<typename C>
inline constexpr auto
rend(const C& c) -> decltype(c.rend())
{
    return c.rend();
}
template<typename C>
inline constexpr auto
crbegin(const C& c) -> decltype(c.rbegin())
{
    return c.rbegin();
}
template<typename C>
inline constexpr auto
crend(const C& c) -> decltype(c.rend())
{
    return c.rend();
}
template<typename T, size_t N>
inline constexpr const T*
cbegin(const T (&arr)[N])
{
    return arr;
}
template<typename T, size_t N>
inline constexpr const T*
cend(const T (&arr)[N])
{
    return arr + N;
}
template<typename T, size_t N> inline constexpr std::reverse_iterator<T*> rbegin(T (&arr)[N]) { return {arr + N}; }
template<typename T, size_t N> inline constexpr std::reverse_iterator<T*> rend(T (&arr)[N]) { return {arr}; }
template<typename T, size_t N>
inline constexpr std::reverse_iterator<const T*>
crbegin(const T (&arr)[N])
{
    return {arr + N};
}
template<typename T, size_t N>
inline constexpr std::reverse_iterator<const T*>
crend(const T (&arr)[N])
{
    return {arr};
}

template<class T, class... Args>
std::unique_ptr<typename concepts::extent_kind<T>::single_object>
make_unique(Args&&... args)
{
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

template<class T>
std::unique_ptr<typename concepts::extent_kind<T>::unknown_bound_array[]>
make_unique(size_t n)
{
    typedef typename std::remove_extent<T>::type U;
    return std::unique_ptr<T>(new U[n]());
}

template<class T> using remove_const_t                 = typename std::remove_const<T>::type;
template<class T> using remove_reference_t             = typename std::remove_reference<T>::type;
template<class T> using make_unsigned_t                = typename std::make_unsigned<T>::type;
template<bool B, class T = void> using enable_if_t     = typename std::enable_if<B, T>::type;
template<bool I, class T, class E> using conditional_t = typename std::conditional_t<I, T, E>::type;

#    define CPP14_CONSTEXPR

#endif // __cplusplus >= 201402L

#if __cplusplus >= 201703L
using std::byte;
using std::conjunction;
using std::empty;
using std::size;

#    define RANGES_FOR(VAR_DECL, ...) for (VAR_DECL : (__VA_ARGS__))
#    define CPP17_NOEXCEPT noexcept
#    define CPP17_STATIC_INLINE_VAR inline
#    define CPP17_IF_CONSTEXPR if constexpr

#else
template<typename C>
inline constexpr auto
size(const C& c) -> decltype(c.size())
{
    return c.size();
}
template<typename C>
inline constexpr auto
empty(const C& c) -> decltype(c.empty())
{
    return c.empty();
}
template<typename T, size_t N>
inline constexpr size_t
size(const T (&)[N])
{
    return N;
}
template<typename T, size_t N>
inline constexpr size_t
empty(const T (&)[N])
{
    return false;
}

enum class byte : unsigned char {};

// Allow to write for range loop where the sentinel type is not the same as the iterator type
// From https://github.com/ericniebler/range-v3/blob/master/include/range/v3/range_for.hpp
//  Copyright Eric Niebler 2014-present
// clang-format off
#define RANGES_FOR(VAR_DECL, ...)                                                                       \
    if(bool _range_gatbl_done = false) {}                                                               \
    else for(auto && _range_gatbl_rng = (__VA_ARGS__); !_range_gatbl_done;)                             \
        for(auto _range_gatbl_begin = gatbl::begin(_range_gatbl_rng); !_range_gatbl_done;               \
                _range_gatbl_done = true)                                                               \
            for(auto _range_gatbl_end = gatbl::end(_range_gatbl_rng);                                   \
                    !_range_gatbl_done && _range_gatbl_begin != _range_gatbl_end; ++_range_gatbl_begin) \
                if(!(_range_gatbl_done = true)) {}                                                      \
                else for(VAR_DECL = *_range_gatbl_begin; _range_gatbl_done; _range_gatbl_done = false)  \
// clang-format on

// C++17 allows noexcept for function pointer types
#define CPP17_NOEXCEPT

#define CPP17_STATIC_INLINE_VAR extern weak_sym

#define CPP17_IF_CONSTEXPR if

#endif // __cplusplus >= 201402L

}

#endif // COMPATIBILITY_HPP
