#ifndef ASSOCIATED_EHANDLERS_TYPE_HPP
#define ASSOCIATED_EHANDLERS_TYPE_HPP

#include <tuple>

namespace async_utils {

namespace detail {

template <typename> struct associated_ehandlers_type_check { using type = void; };

template <typename T, typename E, typename = void> struct associated_ehandlers_type_impl { using type = E; };
template <typename T, typename E>
struct associated_ehandlers_type_impl<T, E, typename associated_ehandlers_type_check<typename T::type_type>::type> {
    using type = typename T::ehandlers_type;
};

} // namespace detail

// Traits type used to obtain the E-handlers type associated with an object.
template <typename T> struct associated_ehandlers_type {
    // If T has a nested type ehandlers_type, T::ehandlers_type.
    /// Otherwise std::tuple<>.
    using type = typename detail::associated_ehandlers_type_impl<T, std::tuple<>>::type;
};

template <typename T> using associated_ehandlers_type_t = typename associated_ehandlers_type<T>::type;

} // namespace async_utils

#endif
