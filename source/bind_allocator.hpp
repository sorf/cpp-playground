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
    const target_type &get() const noexcept { return this->target; }
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
template <typename Allocator, typename T> inline decltype(auto) bind_allocator(Allocator const &allocator, T &&target) {
    auto a = allocator;
    return allocator_binder<typename std::decay_t<T>, Allocator>(std::forward<T>(target), std::move(a));
}

} // namespace async_utils

namespace boost {
namespace asio {

template <typename T, typename Allocator, typename Allocator1>
struct associated_allocator<async_utils::allocator_binder<T, Allocator>, Allocator1> {
    using type = Allocator;
    static type get(async_utils::allocator_binder<T, Allocator> const &b,
                    Allocator1 const & /*unused*/ = Allocator1()) noexcept {
        return b.get_allocator();
    }
};

template <typename T, typename Allocator, typename Executor1>
struct associated_executor<async_utils::allocator_binder<T, Allocator>, Executor1> {
    using type = typename associated_executor<T, Executor1>::type;
    static type get(async_utils::allocator_binder<T, Allocator> const &b, Executor1 const &e = Executor1()) noexcept {
        return associated_executor<T, Executor1>::get(b.get(), e);
    }
};

} // namespace asio
} // namespace boost

#endif
