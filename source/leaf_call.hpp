#ifndef LEAF_CALL_HPP
#define LEAF_CALL_HPP

#include "associated_ehandlers_type.hpp"
#include <boost/leaf/capture_in_result.hpp>
#include <boost/leaf/exception_to_result.hpp>
#include <boost/leaf/preload.hpp>

namespace async_utils {
namespace leaf = boost::leaf;

// Defers a leaf::accumulate call.
template <typename F> decltype(auto) defer_accumulate(F &&f) {
    return [f = std::forward<F>(f)] { return leaf::accumulate(std::move(f)); };
}

// Packs a deferred leaf::accumulate() call.
template <typename LeafAccumulator, typename F> decltype(auto) defer_accumulate(LeafAccumulator &&acc, F &&f) {
    return [prev_acc = std::forward<LeafAccumulator>(acc), f = std::forward<F>(f)] {
        return std::make_pair(prev_acc(), leaf::accumulate(std::move(f)));
    };
}

// Calls a function and captures any errors that occurred according to the passed E-handlers type.
template <typename R, typename EHandlers, typename F> leaf::result<R> leaf_call(F &&f) {
    using result_type = leaf::result<R>;
    return leaf::capture_in_result<EHandlers>([f = std::forward<F>(f)]() mutable -> result_type {
        return leaf::exception_to_result([&]() -> result_type { return f(); });
    });
}
template <typename R, typename EHandlers, typename LeafAccumulator, typename F>
leaf::result<R> leaf_call(LeafAccumulator &&acc, F &&f) {
    using result_type = leaf::result<R>;
    return leaf::capture_in_result<EHandlers>(
        [acc = std::forward<LeafAccumulator>(acc), f = std::forward<F>(f)]() mutable -> result_type {
            [[maybe_unused]] auto accumulate = acc();
            return leaf::exception_to_result([&]() -> result_type { return f(); });
        });
}

// Calls a function and captures any errors that occurred according to the E-handlers type associated with it.
template <typename R, typename F> leaf::result<R> leaf_call(F &&f) {
    using ehandlers_type = async_utils::associated_ehandlers_type_t<F>;
    return leaf_call<R, ehandlers_type>(std::forward<F>(f));
}
template <typename R, typename F, typename LeafAccumulator> leaf::result<R> leaf_call(LeafAccumulator &&acc, F &&f) {
    using ehandlers_type = async_utils::associated_ehandlers_type_t<F>;
    return leaf_call<R, ehandlers_type>(std::forward<LeafAccumulator>(acc), std::forward<F>(f));
}

} // namespace async_utils

#endif
