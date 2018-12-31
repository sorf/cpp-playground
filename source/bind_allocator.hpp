#ifndef BIND_ALLOCATOR_HPP
#define BIND_ALLOCATOR_HPP

#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>

namespace async_utils {

// Associates an object of type T with an allocator.
template <typename T, typename Allocator> struct allocator_binder {
    allocator_binder(T &&target, Allocator &&allocator) : target(std::move(target)), allocator(std::move(allocator)) {}

    using target_type = T;
    using allocator_type = Allocator;

    target_type &get() noexcept { return this->target; }
    const target_type &get() const { return this->target; }
    allocator_type get_allocator() const noexcept { return this->allocator; }

    template <class... Args> decltype(auto) operator()(Args &&... args) const {
        std::invoke(target, std::forward<Args>(args)...);
    }

    template <class... Args> decltype(auto) operator()(Args &&... args) {
        std::invoke(target, std::forward<Args>(args)...);
    }

    target_type target;
    allocator_type allocator;
};

// Associates an object of type T with an allocator.
// Similar to boost::asio::bind_executor
template <typename Allocator, typename T> inline decltype(auto) bind_allocator(Allocator &&allocator, T &&target) {
    return allocator_binder<typename std::decay_t<T>, Allocator>(std::forward<T>(target),
                                                                 std::forward<Allocator>(allocator));
}

} // namespace async_utils

namespace boost {
namespace asio {

template <typename T, typename Allocator, typename Allocator1>
struct associated_allocator<async_utils::allocator_binder<T, Allocator>, Allocator1> {
    using type = Allocator;
    static type get(const async_utils::allocator_binder<T, Allocator> &b,
                    const Allocator1 & /*unused*/ = Allocator1()) noexcept {
        return b.get_allocator();
    }
};

template <typename T, typename Allocator, typename Executor1>
struct associated_executor<async_utils::allocator_binder<T, Allocator>, Executor1> {
    using type = typename associated_executor<T, Executor1>::type;
    static type get(const async_utils::allocator_binder<T, Allocator> &b, const Executor1 &e = Executor1()) noexcept {
        return associated_executor<T, Executor1>::get(b.get(), e);
    }
};

} // namespace asio
} // namespace boost

#endif
