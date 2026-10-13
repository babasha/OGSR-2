////////////////////////////////////////////////////////////////////////////
//	Module 		: object_type_traits.h
//	Created 	: 21.01.2003
//  Modified 	: 12.07.2004
//	Author		: Dmitriy Iassenev
//	Description : Object type traits
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <type_traits>

#define declare_has(a) \
    template <typename T> \
    struct has_##a \
    { \
        template <typename P> \
        static std::true_type select(detail::other<typename P::a>*); \
        template <typename P> \
        static std::false_type select(...); \
        static constexpr auto value = std::is_same<std::true_type, decltype(select<T>(nullptr))>::value; \
    };

// A full specialization of a nested member template (selector<true> below,
// inside _if<expression, T1, T2>) is only legal at namespace scope, not
// inside the enclosing class template's body -- MSVC accepts it as a
// leniency, GCC/Clang refuse it ("explicit specialization in non-namespace
// scope"). Rewritten as a partial specialization of _if itself instead
// (legal at namespace scope), same result type either way.
template <bool expression, typename T1, typename T2>
struct _if
{
    typedef T2 result;
};

template <typename T1, typename T2>
struct _if<true, T1, T2>
{
    typedef T1 result;
};

namespace object_type_traits
{
namespace detail
{
template <typename T>
struct other
{};
} // namespace detail

using std::is_const;
using std::is_pointer;
using std::is_reference;
using std::is_same;
using std::is_void;
using std::remove_const;
using std::remove_pointer;
using std::remove_reference;

namespace detail
{
template <typename T, typename U>
struct is_base_and_derived : std::conjunction<std::is_class<T>, std::is_class<U>, std::is_base_of<T, U>>
{};
} // namespace detail

template <typename T, typename U>
struct is_base_and_derived : detail::is_base_and_derived<std::remove_const_t<T>, std::remove_const_t<U>>
{};

template <typename T, typename U>
constexpr bool is_base_and_derived_v = is_base_and_derived<T, U>::value;

template <template <typename> class T1, typename T2>
struct is_base_and_derived_or_same_from_template
{
    template <typename P>
    static std::true_type select(T1<P>*);
    static std::false_type select(...);

    static constexpr auto value = is_same<std::true_type, decltype(select(static_cast<T2*>(nullptr)))>::value;
};

declare_has(iterator);
declare_has(const_iterator);
declare_has(value_type);
declare_has(size_type);

template <typename T>
struct is_stl_container
{
    enum
    {
        value = has_iterator<T>::value && has_const_iterator<T>::value && has_size_type<T>::value && has_value_type<T>::value
    };
};
}; // namespace object_type_traits
