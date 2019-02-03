#ifndef BIND_EHANDLERS_TYPE_HPP
#define BIND_EHANDLERS_TYPE_HPP

#include "associated_ehandlers_type.hpp"
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>

namespace async_utils {

// Associates an object of type T with an E-handlers type.
template <typename T, typename EHandlers> struct ehandlers_type_binder {
    explicit ehandlers_type_binder(T &&target) : target{std::move(target)} {}

    using target_type = T;
    using ehandlers_type = EHandlers;

    target_type &get() noexcept { return this->target; }
    target_type const &get() const noexcept { return this->target; }

    template <class... Args> decltype(auto) operator()(Args &&... args) const {
        return std::invoke(target, std::forward<Args>(args)...);
    }

    template <class... Args> decltype(auto) operator()(Args &&... args) {
        return std::invoke(target, std::forward<Args>(args)...);
    }

    target_type target;
};

// Associates an object of type T with an E-handlers type.
template <typename EHandlers, typename T>
inline decltype(auto) bind_ehandlers_type(T &&target, EHandlers const * /*unused*/ = nullptr) {
    return ehandlers_type_binder<typename std::decay_t<T>, EHandlers>(std::forward<T>(target));
}
// Associates an object of type T with an E-handlers type.
template <typename EHandlers, typename T>
inline decltype(auto) bind_ehandlers_type(T &&target, EHandlers const & /*unused*/) {
    return ehandlers_type_binder<typename std::decay_t<T>, EHandlers>(std::forward<T>(target));
}

// Associates an object of type T with an E-handlers type retrieved from another type.
template <typename From, typename T>
inline decltype(auto) bind_ehandlers_type_from(T &&target, From const * /*unused*/ = nullptr) {
    using ehandlers_type = associated_ehandlers_type_t<From>;
    return bind_ehandlers_type<ehandlers_type>(std::forward<T>(target));
}
// Associates an object of type T with an E-handlers type retrieved from another type.
template <typename From, typename T>
inline decltype(auto) bind_ehandlers_type_from(T &&target, From const & /*unused*/) {
    using ehandlers_type = associated_ehandlers_type_t<From>;
    return bind_ehandlers_type<ehandlers_type>(std::forward<T>(target));
}

template <typename T, typename EHandlers> struct associated_ehandlers_type<ehandlers_type_binder<T, EHandlers>> {
    using type = EHandlers;
};

} // namespace async_utils

namespace boost {
namespace asio {

template <typename T, typename EHandlers, typename Allocator1>
struct associated_allocator<async_utils::ehandlers_type_binder<T, EHandlers>, Allocator1> {
    using type = typename associated_allocator<T, Allocator1>::type;
    static type get(async_utils::ehandlers_type_binder<T, EHandlers> const &b,
                    Allocator1 const &a = Allocator1()) noexcept {
        return associated_allocator<T, Allocator1>::get(b.get(), a);
    }
};

template <typename T, typename EHandlers, typename Executor1>
struct associated_executor<async_utils::ehandlers_type_binder<T, EHandlers>, Executor1> {
    using type = typename associated_executor<T, Executor1>::type;
    static type get(async_utils::ehandlers_type_binder<T, EHandlers> const &b,
                    Executor1 const &e = Executor1()) noexcept {
        return associated_executor<T, Executor1>::get(b.get(), e);
    }
};

} // namespace asio
} // namespace boost

#endif
