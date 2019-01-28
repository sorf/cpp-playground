#ifndef BIND_ERROR_HANDLER_HPP
#define BIND_ERROR_HANDLER_HPP

#include "associated_error_handler.hpp"
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>

namespace async_utils {

// Associates an object of type T with an error-handler.
template <typename T, typename ErrorHandler> struct error_handler_binder {
    error_handler_binder(T &&target) : target(std::move(target)) {}

    using target_type = T;
    using error_handler_type = ErrorHandler;

    target_type &get() noexcept { return this->target; }
    const target_type &get() const noexcept { return this->target; }

    template <class... Args> decltype(auto) operator()(Args &&... args) const {
        std::invoke(target, std::forward<Args>(args)...);
    }

    template <class... Args> decltype(auto) operator()(Args &&... args) {
        std::invoke(target, std::forward<Args>(args)...);
    }

    target_type target;
};

// Associates an object of type T with an error-handler type.
template <typename ErrorHandler, typename T> inline decltype(auto) bind_error_handler(T &&target) {
    return error_handler_binder<typename std::decay_t<T>, ErrorHandler>(std::forward<T>(target));
}

template <typename T, typename ErrorHandler> struct associated_error_handler<error_handler_binder<T, ErrorHandler>> {
    using type = ErrorHandler;
};

} // namespace async_utils

namespace boost {
namespace asio {

template <typename T, typename Allocator1>
struct associated_allocator<async_utils::associated_error_handler<T>, Allocator1> {
    using type = typename associated_allocator<T, Allocator1>::type;
    static type get(async_utils::associated_error_handler<T> const &b, Allocator1 const &a = Allocator1()) noexcept {
        return associated_allocator<T, Allocator1>::get(b.get(), a);
    }
};

template <typename T, typename Executor1>
struct associated_executor<async_utils::associated_error_handler<T>, Executor1> {
    using type = typename associated_executor<T, Executor1>::type;
    static type get(async_utils::associated_error_handler<T> const &b, Executor1 const &e = Executor1()) noexcept {
        return associated_executor<T, Executor1>::get(b.get(), e);
    }
};

} // namespace asio
} // namespace boost

#endif
