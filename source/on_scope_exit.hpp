#ifndef ON_SCOPE_EXIT_HPP
#define ON_SCOPE_EXIT_HPP

#include <boost/log/utility/unique_identifier_name.hpp>
#include <optional>
#include <utility>

namespace async_utils {

template <typename F> struct [[nodiscard]] scope_exit {
    explicit scope_exit(F && f) : m_f(std::move(f)) {}
    scope_exit(scope_exit const &) = delete;
    scope_exit(scope_exit && other) noexcept : m_f(std::move(other.m_f)) { other.m_f.reset(); }
    scope_exit &operator=(scope_exit const &) = delete;
    scope_exit &operator=(scope_exit &&other) noexcept {
        reset();
        m_f = std::move(other.m_f);
        other.m_f.reset();
        return *this;
    }

    ~scope_exit() { reset(); }

    void reset() noexcept {
        if (m_f) {
            auto f = std::move(m_f);
            m_f.reset(); // Note: a moved-from optional still contains a value
            (*f)();
        }
    }

  private:
    std::optional<F> m_f;
};

// Executes a lambda at scope exit.
// To be used instead of Boost.ScopeExit which may allocate memory as a result of using Boost.Function in its
// implementation.
template <typename F> decltype(auto) on_scope_exit(F &&f) { return scope_exit<F>(std::forward<F>(f)); }

} // namespace async_utils

#define ON_SCOPE_EXIT(f)                                                                                               \
    [[maybe_unused]] auto BOOST_LOG_UNIQUE_IDENTIFIER_NAME(on_scope_exit_guard) = ::async_utils::on_scope_exit(f)

#endif
