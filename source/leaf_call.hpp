#ifndef LEAF_CALL_HPP
#define LEAF_CALL_HPP

#include "associated_ehandlers_type.hpp"
#include <boost/leaf/capture_in_result.hpp>
#include <boost/leaf/exception_to_result.hpp>

namespace async_utils {
namespace leaf = boost::leaf;

// Calls a function and captures any errors that occurred according to the passed E-handlers type.
template <typename R, typename EHandlers, typename F> leaf::result<R> leaf_call(F &&f) {
    using result_type = leaf::result<R>;
    auto cf = leaf::capture_in_result<EHandlers>(
        [&]() -> result_type { return leaf::exception_to_result([&]() -> result_type { return f(); }); });
    return cf();
}

// Calls a function and captures any errors that occurred according to the E-handlers type associated with it.
template <typename R, typename F> leaf::result<R> leaf_call(F &&f) {
    using ehandlers_type = async_utils::associated_ehandlers_type_t<F>;
    return leaf_call<R, ehandlers_type>(std::forward<F>(f));
}

} // namespace async_utils

#endif
