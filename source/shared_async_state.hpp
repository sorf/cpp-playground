#ifndef SHARED_ASYNC_STATE_HPP
#define SHARED_ASYNC_STATE_HPP

#include "bind_allocator.hpp"
#include "on_scope_exit.hpp"

#include <atomic>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/assert.hpp>
#include <memory>
#include <tuple>

namespace async_utils {
namespace asio = boost::asio;

// Default allocator if none associated with the completion handler.
using default_handler_allocator = std::allocator<void>;

// The allocator associated with a completion handler.
template <typename CompletionHandler>
using completion_handler_allocator = asio::associated_allocator_t<CompletionHandler, default_handler_allocator>;

// The allocator associated with a completion handler.
template <typename CompletionHandlerSignature, typename CompletionToken>
using completion_handler_allocator_token = completion_handler_allocator<
    typename asio::async_completion<CompletionToken, CompletionHandlerSignature>::completion_handler_type>;

// The allocator associated with the completion handler rebound for a different type.
template <typename T, typename CompletionHandler>
using define_handler_allocator =
    typename std::allocator_traits<completion_handler_allocator<CompletionHandler>>::template rebind_alloc<T>;

// The allocator associated with the completion handler rebound for a different type.
template <typename T, typename CompletionHandlerSignature, typename CompletionToken>
using define_handler_allocator_token = typename std::allocator_traits<
    completion_handler_allocator_token<CompletionHandlerSignature, CompletionToken>>::template rebind_alloc<T>;

// Returns the allocator associated with the handler rebound for a different type.
template <typename T, typename CompletionHandler>
decltype(auto) get_handler_allocator(CompletionHandler const &handler) {
    define_handler_allocator<T, CompletionHandler> allocator =
        asio::get_associated_allocator(handler, default_handler_allocator{});
    return allocator;
}

// The executor associated associated with a completion handler.
template <typename CompletionHandler, typename Executor>
using completion_handler_executor = typename asio::associated_executor<CompletionHandler, Executor>::type;

// The executor associated associated with a completion handler.
template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor>
using completion_handler_executor_token = completion_handler_executor<
    typename asio::async_completion<CompletionToken, CompletionHandlerSignature>::completion_handler_type, Executor>;

// Container of the state associated with multiple asychronous operations.
// It holds the final operation completion handler, an executor to be used as a default if the final completion handler
// doesn't have any associated with it and any other state data used by the implementation of the asychronous operation.
// The memory used to hold the state is managed using the allocator associated with the completion handler.
template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor, typename StateData>
class shared_async_state {
  public:
    // Completion type.
    using completion_type = asio::async_completion<CompletionToken, CompletionHandlerSignature>;
    // Completion handler type generated from CompletionHandlerSignature.
    using completion_handler_type = typename completion_type::completion_handler_type;

    // The allocator associated with the completion handler rebound for a different type.
    template <typename T>
    using define_handler_allocator =
        ::async_utils::define_handler_allocator_token<T, CompletionHandlerSignature, CompletionToken>;

    // The executor associated with the completion handler
    using completion_handler_executor_type =
        completion_handler_executor_token<CompletionHandlerSignature, CompletionToken, Executor>;

  private:
    struct state_holder;
    using state_allocator_type = define_handler_allocator<state_holder>;
    using state_allocator_traits = std::allocator_traits<state_allocator_type>;

  public:
    // Constructor.
    //
    // It creates a state object from a completion handler, default executor and arguments to construct the state
    // data.
    template <typename... Args>
    explicit shared_async_state(Executor &&executor, completion_handler_type &&completion_handler, Args &&... args)
        : m_state{} {
        auto state_allocator{get_handler_allocator<state_holder>(completion_handler)};
        auto p = state_allocator_traits::allocate(state_allocator, 1);
        bool commit = false;
        ON_SCOPE_EXIT([&] {
            if (!commit) {
                state_allocator_traits::deallocate(state_allocator, p, 1);
                p = nullptr;
            }
        });
        state_allocator_traits::construct(state_allocator, std::addressof(*p), std::move(executor),
                                          std::move(completion_handler), std::forward<Args>(args)...);

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
    template <typename T> decltype(auto) get_allocator() const noexcept {
        return get_handler_allocator<T>(m_state->completion_handler);
    }

    // Returns the executor associated with the handler.
    completion_handler_executor_type get_executor() const noexcept {
        return asio::get_associated_executor(m_state->completion_handler, m_state->executor);
    }

    // Wrap a completion handler with the executor and allocator associated with the final completion handler.
    template <typename F> decltype(auto) wrap(F &&f) const {
        BOOST_ASSERT(m_state);
        BOOST_ASSERT(m_state->io_work.owns_work());
        auto allocator = this->get_allocator<void>();
        auto executor = this->get_executor();
        // Note: bind_executor() followed by bind_allocator() fails to compile if the result of this wrap() function
        // is bound to a strand, e.g. if calling:
        //      asio::bind_executor(strand, wrap(...))
        return bind_allocator(std::move(allocator), asio::bind_executor(std::move(executor), std::forward<F>(f)));
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

  private:
    completion_handler_type release() noexcept {
        BOOST_ASSERT(m_state);
        auto state_allocator = get_allocator<state_holder>();
        completion_handler_type completion_handler = std::move(m_state->completion_handler);
        state_allocator_traits::destroy(state_allocator, std::addressof(*m_state));
        state_allocator_traits::deallocate(state_allocator, m_state, 1);
        m_state = nullptr;
        return std::move(completion_handler);
    }

    struct state_holder {
        template <typename... Args>
        state_holder(Executor &&executor_arg, completion_handler_type &&completion_handler, Args &&... args)
            : executor(std::move(executor_arg)),
              completion_handler(std::move(completion_handler)), io_work{asio::make_work_guard(executor)}, ref_count{1},
              state_data{std::forward<Args>(args)...} {}

        state_holder(state_holder const &) = delete;
        state_holder(state_holder &&) = delete;
        state_holder &operator=(state_holder const &) = delete;
        state_holder &operator=(state_holder &&) = delete;
        ~state_holder() = default;

        Executor executor;
        completion_handler_type completion_handler;
        asio::executor_work_guard<Executor> io_work;
        std::atomic_size_t ref_count;
        StateData state_data;
    };
    state_holder *m_state;
};

} // namespace async_utils

#endif
