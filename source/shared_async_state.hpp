#ifndef SHARED_ASYNC_STATE_HPP
#define SHARED_ASYNC_STATE_HPP

#include "bind_allocator.hpp"
#include "handler_traits.hpp"
#include "on_scope_exit.hpp"

#include <atomic>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/assert.hpp>
#include <tuple>

#if defined(ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT)
#include <boost/format.hpp>
#include <boost/log/utility/unique_identifier_name.hpp>
#include <iostream>
#if defined(ASYNC_UTILS_ENABLE_DEBUG_DELAYS)
#include <chrono>
#include <mutex>
#include <random>
#include <thread>
#endif // ASYNC_UTILS_ENABLE_DEBUG_DELAYS
#endif // ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT

namespace async_utils {
namespace asio = boost::asio;

namespace detail {
void debug_delay();
} // namespace detail

// Container of the state associated with multiple asychronous operations.
// It holds the final operation completion handler, an executor to be used as a default if the final completion handler
// doesn't have any associated with it and any other state data used by the implementation of the asychronous operation.
// The memory used to hold the state is managed using the allocator associated with the completion handler.
template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor, typename StateData>
class shared_async_state {
    using h_traits = handler_traits<CompletionHandlerSignature, CompletionToken, Executor>;

  public:
    // Completion type.
    using completion_type = typename h_traits::completion_type;
    // Completion handler type generated from CompletionHandlerSignature.
    using handler_type = typename h_traits::handler_type;

    // The allocator associated with the completion handler rebound for a different type.
    template <typename T> using rebound_allocator_type = typename h_traits::template rebound_allocator_type<T>;

    // The executor associated with the completion handler
    using handler_executor_type = typename h_traits::handler_executor_type;

  private:
    struct state_holder;
    using state_allocator_type = rebound_allocator_type<state_holder>;
    using state_allocator_traits = std::allocator_traits<state_allocator_type>;

  public:
    // Constructor.
    //
    // It creates a state object from a completion handler, default executor and arguments to construct the state
    // data.
    template <typename... Args>
    explicit shared_async_state(Executor const &executor, handler_type &&handler, Args &&... args) : m_state{} {
        auto state_allocator{h_traits::template get_handler_allocator<state_holder>(handler)};
        auto p = state_allocator_traits::allocate(state_allocator, 1);
        bool commit = false;
        ON_SCOPE_EXIT([&] {
            if (!commit) {
                state_allocator_traits::deallocate(state_allocator, p, 1);
                p = nullptr;
            }
        });
        state_allocator_traits::construct(state_allocator, std::addressof(*p), executor, std::move(handler),
                                          std::forward<Args>(args)...);

        ON_SCOPE_EXIT([&] {
            if (!commit) {
                state_allocator_traits::destroy(state_allocator, std::addressof(*p));
            }
        });

        m_state = p;
        commit = true;
    }

    // Copy constructor.
    // This new instance will share ownership of the state data with the other instance.
    shared_async_state(shared_async_state const &other) noexcept : m_state(other.m_state) {
        if (m_state != nullptr) {
            ++m_state->ref_count;
        }
    }
    // Move constructor.
    shared_async_state(shared_async_state &&other) noexcept : m_state{} { std::swap(m_state, other.m_state); }

    shared_async_state &operator=(shared_async_state const &) = delete;
    shared_async_state &operator=(shared_async_state &&other) = delete;
    ~shared_async_state() { reset(); }

    bool has_data() const noexcept { return m_state != nullptr; }

    StateData &get_data() noexcept {
        BOOST_ASSERT(m_state);
        return m_state->state_data;
    }
    StateData const &get_data() const noexcept {
        BOOST_ASSERT(m_state);
        return m_state->state_data;
    }

    // Returns the allocator associated with the handler rebound for a different type.
    template <typename T> decltype(auto) get_handler_allocator() const {
        return h_traits::template get_handler_allocator<T>(m_state->handler);
    }

    // Returns the executor associated with the handler.
    handler_executor_type get_handler_executor() const {
        return h_traits::get_handler_executor(m_state->handler, m_state->executor);
    }

    // Wrap a completion handler with the executor and allocator associated with the final completion handler.
    template <typename F> decltype(auto) wrap(F &&f) const {
        BOOST_ASSERT(m_state);
        BOOST_ASSERT(m_state->io_work.owns_work());

        auto allocator = this->get_handler_allocator<void>();
        auto executor = this->get_handler_executor();
        return asio::bind_executor(executor, bind_allocator(std::move(allocator), std::forward<F>(f)));
    }

    // Wrap a completion handler with the given executor and the allocator associated with the final completion handler.
    template <typename OtherExecutor, typename F> decltype(auto) wrap(OtherExecutor const &executor, F &&f) const {
        BOOST_ASSERT(m_state);
        BOOST_ASSERT(m_state->io_work.owns_work());

        auto allocator = this->get_handler_allocator<void>();
        return asio::bind_executor(executor, bind_allocator(std::move(allocator), std::forward<F>(f)));
    }

    // Tests that this is the only instance sharing the state data.
    bool unique() const noexcept { return m_state != nullptr && m_state->ref_count == 1; }

    // Invokes the final completion handler with the given arguments.
    // This should be called at most once and only if this is unique().
    // The state is deallocated before calling the handler, so the passsed arguments should not refer any data
    // from this state.
    template <typename... Args> void invoke(Args &&... args) {
        BOOST_ASSERT(unique());
        auto handler = release();
        std::invoke(handler, std::forward<Args>(args)...);
    }

    // Invokes the final completion handler after moving the given arguments.
    // This method ensures that the handler is not called with references to the state data.
    template <typename... Args> void invoke_move_args(Args &&... args) {
        BOOST_ASSERT(unique());
        auto f = [this](auto &&... a) { this->invoke(a...); };
        auto t = std::make_tuple(std::forward<Args>(args)...);
        std::apply(f, t);
    }

    // Conditionally invokes the final completion handler after moving the given arguments.
    // Does nothing if this is not unique.
    template <typename... Args> bool try_invoke_move_args(Args &&... args) {
        if (unique()) {
            this->invoke_move_args(std::forward<Args>(args)...);
            return true;
        }
        return false;
    }

    void reset() noexcept {
        if (m_state != nullptr) {
            if (--m_state->ref_count == 0) {
                release();
            } else {
                m_state = nullptr;
            }
        }
    }

    // Returns a scope guard object that ensures there are no concurrent accesses until its destruction or
    // invoke() is called.
    decltype(auto) debug_check_not_concurrent(char const *file = "", int line = 0) {
        BOOST_ASSERT(m_state);
        return debug_check_not_concurrent_scope_exit(*m_state, file, line);
    }

  private:
    handler_type release() noexcept {
        BOOST_ASSERT(m_state);
#if defined(ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT)
        if (debug_check_not_concurrent_scope_exit *p = m_state->debug_check_not_concurrent_scope_exit_p) {
            p->reset();
        }
        BOOST_ASSERT(m_state->debug_check_not_concurrent_scope_exit_p == nullptr);
#endif

        auto state_allocator = get_handler_allocator<state_holder>();
        handler_type handler = std::move(m_state->handler);
        state_allocator_traits::destroy(state_allocator, std::addressof(*m_state));
        state_allocator_traits::deallocate(state_allocator, m_state, 1);
        m_state = nullptr;
        return handler;
    }

#if defined(ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT)
    struct debug_check_not_concurrent_scope_exit;
#endif
    struct state_holder {
        template <typename... Args>
        state_holder(Executor const &executor, handler_type &&handler, Args &&... args)
            : executor(executor), handler(std::move(handler)), io_work{asio::make_work_guard(executor)}, ref_count{1},
#if defined(ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT)
              debug_check_not_concurrent_scope_exit_p{},
#endif
              state_data{std::forward<Args>(args)...} {
        }

        state_holder(state_holder const &) = delete;
        state_holder(state_holder &&) = delete;
        state_holder &operator=(state_holder const &) = delete;
        state_holder &operator=(state_holder &&) = delete;
        ~state_holder() = default;

        Executor executor;
        handler_type handler;
        asio::executor_work_guard<Executor> io_work;
        std::atomic_size_t ref_count;
#if defined(ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT)
        std::atomic<debug_check_not_concurrent_scope_exit *> debug_check_not_concurrent_scope_exit_p;
#endif
        StateData state_data;
    };

#if defined(ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT)
    struct [[nodiscard]] debug_check_not_concurrent_scope_exit {
        explicit debug_check_not_concurrent_scope_exit(state_holder & state, char const *file, std::size_t line)
            : m_state(&state), m_file(file), m_line(line) {
            auto *was_executing = m_state->debug_check_not_concurrent_scope_exit_p.exchange(this);
            if (was_executing != nullptr) {
                print_concurrent_execution_error(was_executing);
                BOOST_ASSERT(was_executing == nullptr);
            }
            detail::debug_delay();
        }

        debug_check_not_concurrent_scope_exit(debug_check_not_concurrent_scope_exit const &) = delete;
        debug_check_not_concurrent_scope_exit(debug_check_not_concurrent_scope_exit &&) = delete;
        debug_check_not_concurrent_scope_exit &operator=(debug_check_not_concurrent_scope_exit const &) = delete;
        debug_check_not_concurrent_scope_exit &operator=(debug_check_not_concurrent_scope_exit &&other) = delete;
        ~debug_check_not_concurrent_scope_exit() { reset(); }

        void reset() noexcept {
            if (m_state) {
                detail::debug_delay();
                auto *was_executing = m_state->debug_check_not_concurrent_scope_exit_p.exchange(nullptr);
                if (was_executing != this) {
                    print_concurrent_execution_error(was_executing);
                    BOOST_ASSERT(was_executing == this);
                }
                m_state = nullptr;
            }
        }

      private:
        void print_concurrent_execution_error(debug_check_not_concurrent_scope_exit * other) {
            std::cerr << boost::format("Unexpected concurrent execution between:\n\t%1%:%2%\n\t%3%:%4%") % m_file %
                             m_line % (other ? other->m_file : "") % (other ? other->m_line : 0)
                      << std::endl;
        }
        state_holder *m_state;
        char const *m_file;
        std::size_t m_line;
    };
#endif // ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT

    state_holder *m_state;
};

#if defined(ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT)
#define DEBUG_CHECK_NOT_CONCURRENT_IMPL()                                                                              \
    [[maybe_unused]] auto BOOST_LOG_UNIQUE_IDENTIFIER_NAME(debug_check_not_concurrent_guard) =                         \
        debug_check_not_concurrent(__FILE__, __LINE__)
#else
#define DEBUG_CHECK_NOT_CONCURRENT_IMPL() (void)0
#endif

// Calls debug_check_not_concurrent().
// Note: This should call this->debug_check_not_concurrent() but this fails to compile in MSVC when called
// from lambda functions that copy this as value.
#define DEBUG_CHECK_NOT_CONCURRENT() DEBUG_CHECK_NOT_CONCURRENT_IMPL()

namespace detail {

void debug_delay() {
#if defined(ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT) && defined(ASYNC_UTILS_ENABLE_DEBUG_DELAYS)
    struct delay_generator {
        delay_generator() : distribution(0, 100) {}
        unsigned next() {
            std::lock_guard<std::mutex> lock{mutex};
            return distribution(rd);
        }

        std::mutex mutex;
        std::random_device rd;
        std::uniform_int_distribution<unsigned> distribution;
    };
    static delay_generator s_delay_generator;
    auto delay = s_delay_generator.next();
    std::this_thread::sleep_for(std::chrono::milliseconds{delay});
#endif // ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT && ASYNC_UTILS_ENABLE_DEBUG_DELAYS
}

} // namespace detail

} // namespace async_utils

#endif
