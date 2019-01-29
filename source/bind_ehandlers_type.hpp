#ifndef BIND_EHANDLERS_TYPE_HPP
#define BIND_EHANDLERS_TYPE_HPP

#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>

namespace async_utils {

namespace detail {

template <typename> struct associated_ehandlers_type_check { typedef void type; };

template <typename T, typename E, typename = void> struct associated_ehandlers_type_impl { typedef E type; };
template <typename T, typename E>
struct associated_ehandlers_type_impl<T, E, typename associated_ehandlers_type_check<typename T::type_type>::type> {
    typedef typename T::ehandlers_type type;
};

} // namespace detail

// Traits type used to obtain the E-handlers type associated with an object.
template <typename T> struct associated_ehandlers_type {
    // If T has a nested type ehandlers_type, T::ehandlers_type.
    /// Otherwise std::tuple<>.
    using type = typename detail::associated_ehandlers_type_impl<T, std::tuple<>>::type;
};

template <typename T> using associated_ehandlers_type_t = typename associated_ehandlers_type<T>::type;

// Associates an object of type T with an E-handlers type.
template <typename T, typename EHandlers> struct ehandlers_type_binder {
    ehandlers_type_binder(T &&target) : target(std::move(target)) {}

    using target_type = T;
    using ehandlers_type = EHandlers;

    target_type &get() noexcept { return this->target; }
    const target_type &get() const noexcept { return this->target; }

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
inline decltype(auto) bind_ehandlers_type(T &&target, EHandlers const * = nullptr) {
    return ehandlers_type_binder<typename std::decay_t<T>, EHandlers>(std::forward<T>(target));
}
// Associates an object of type T with an E-handlers type.
template <typename EHandlers, typename T> inline decltype(auto) bind_ehandlers_type(T &&target, EHandlers const &) {
    return ehandlers_type_binder<typename std::decay_t<T>, EHandlers>(std::forward<T>(target));
}

// Associates an object of type T with an E-handlers type retrieved from another type.
template <typename From, typename T>
inline decltype(auto) bind_ehandlers_type_from(T &&target, From const * = nullptr) {
    using ehandlers_type = associated_ehandlers_type_t<From>;
    return bind_ehandlers_type<ehandlers_type>(std::forward<T>(target));
}
// Associates an object of type T with an E-handlers type retrieved from another type.
template <typename From, typename T> inline decltype(auto) bind_ehandlers_type_from(T &&target, From const &) {
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
