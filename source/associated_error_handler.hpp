#ifndef ASSOCIATED_ERROR_HANDLER_HPP
#define ASSOCIATED_ERROR_HANDLER_HPP

#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <tuple>

namespace async_utils {

namespace detail {

template <typename> struct associated_error_handler_check { typedef void type; };

template <typename T, typename E, typename = void> struct associated_error_handler_impl { typedef E type; };

template <typename T, typename E>
struct associated_error_handler_impl<T, E,
                                     typename associated_error_handler_check<typename T::error_handler_type>::type> {
    typedef typename T::error_handler_type type;
};

} // namespace detail

// Traits type used to obtain the associated error handler with an object.
template <typename T> struct associated_error_handler {
    // If T has a nested type error_handler_type, T::error_handler_type.
    /// Otherwise std::tuple<>.
    using type = typename detail::associated_error_handler_impl<T, std::tuple<>>::type;
};

template <typename T> using associated_error_handler_t = typename associated_error_handler<T>::type;

} // namespace async_utils

#endif
