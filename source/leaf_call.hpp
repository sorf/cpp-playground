#ifndef LEAF_CALL_HPP
#define LEAF_CALL_HPP

#include "associated_ehandlers_type.hpp"
#include <boost/leaf/capture_in_result.hpp>
#include <boost/leaf/exception_to_result.hpp>

namespace async_utils {
namespace leaf = boost::leaf;

// Calls a function and captures any errors that occurred according to the passed error-handler type.
template <typename R, typename ErrorHandler, typename F> leaf::result<R> leaf_call(F &&f) {
    using result_type = leaf::result<R>;
    return leaf::capture_in_result<ErrorHandler>([f = std::forward<F>(f)]() mutable -> result_type {
        return leaf::exception_to_result([&]() -> result_type { return f(); });
    });
}

// Calls a function and captures any errors that occurred according to the passed E-handlers type.
template <typename R, typename From, typename F> leaf::result<R> leaf_call_from(From const & /*unused*/, F &&f) {
    using result_type = leaf::result<R>;
    using error_handler_type = async_utils::associated_ehandlers_type_t<From>;
    return leaf_call<R, error_handler_type>(std::forward<F>(f));

    return leaf::capture_in_result<error_handler_type>([f = std::forward<F>(f)]() mutable -> result_type {
        return leaf::exception_to_result([&]() -> result_type { return f(); });
    });
}

} // namespace async_utils

#endif
