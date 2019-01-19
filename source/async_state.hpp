#ifndef ASYNC_STATE_HPP
#define ASYNC_STATE_HPP

#include "bind_allocator.hpp"

#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/scope_exit.hpp>
#include <memory>

namespace async_utils {
namespace asio = boost::asio;

template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor, typename StateData>
class async_state;

// Container of the state data associated with asychronous operations.
// It holds the final operation completion handler, an executor to be used as a default if the final completion handler
// doesn't have any associated with it and any other state data used by the implementation of the asychronous operation.
// The memory used to hold the state is managed using the allocator associated with the completion handler.
template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor, typename StateData>
class async_state {

    struct state_holder;

  public:
    // Completion type.
    using completion_type = asio::async_completion<CompletionToken, CompletionHandlerSignature>;
    // Completion handler type generated from CompletionHandlerSignature.
    using completion_handler_type = typename completion_type::completion_handler_type;
    // Default allocator if none associated with the completion handler.
    using default_allocator = std::allocator<void>;
    // The allocator associated with the completion handler.
    using completion_handler_allocator_type = asio::associated_allocator_t<completion_handler_type, default_allocator>;

    // Constructor.
    //
    // It creates a state object from a completion handler, default executor and arguments to construct the state data.
    template <class... Args>
    explicit async_state(completion_handler_type &&completion_handler, Executor const &executor, Args &&... args)
        : m_state{async_state::make_state(std::move(completion_handler), executor, std::forward<Args>(args)...)} {}

    async_state(async_state const &) = delete;
    async_state(async_state &&) noexcept = default;
    async_state &operator=(async_state const &) = delete;
    async_state &operator=(async_state &&) noexcept = default;
    ~async_state() = default;

    // Checks if there is any state set.
    bool has_data() const noexcept { return m_state != nullptr; }

    // Returns a pointer to the owned state data.
    StateData *get_data() const {
        BOOST_ASSERT(m_state);
        return m_state.get();
    }

    // Object returned by wrap() that allows adapting a (lambda) function object to be a proper completion handler.
    // This object insures that state object can be safely moved when building the lambda intermediary completion
    // handler.
    class wrapper {
      public:
        explicit wrapper(state_holder *state) : m_state{state} {}
        template <typename F> decltype(auto) operator()(F &&f) const {
            // The final completion handler should not have been called yet.
            BOOST_ASSERT(m_state);
            BOOST_ASSERT(m_state->io_work.owns_work());
            auto allocator = asio::get_associated_allocator(m_state->completion_handler, default_allocator{});
            auto executor = asio::get_associated_executor(m_state->completion_handler, m_state->executor);
            return asio::bind_executor(executor, bind_allocator(std::move(allocator), std::forward<F>(f)));
        }

      private:
        state_holder *m_state;
    };

    // Returns an object referring this state object that can be used to wrap intermediary (lambda) completion handlers.
    // After calling this method, the state object can be safely moved when building a lambda intermediary completion
    // handler.
    wrapper wrap() { return wrapper{m_state.get()}; }

    // Invokes the final completion handler with the given arguments.
    // This should be called at most once.
    // The state and associated state data is deallocated before calling the handler.
    template <class... Args> void invoke(Args &&... args) {
        BOOST_ASSERT(m_state);
        auto completion_handler = std::move(m_state->completion_handler);
        m_state.reset();
        std::invoke(completion_handler, std::forward<Args>(args)...);
    }

  private:
    struct state_holder_base {
        state_holder_base(completion_handler_type &&completion_handler, Executor const &executor)
            : completion_handler(std::move(completion_handler)),
              executor(executor), io_work{asio::make_work_guard(executor)} {}

        completion_handler_type completion_handler;
        Executor executor;
        asio::executor_work_guard<Executor> io_work;
    };

    struct state_holder : public state_holder_base, public StateData {
        template <class... Args>
        state_holder(completion_handler_type &&completion_handler, Executor const &executor, Args &&... args)
            : state_holder_base(std::move(completion_handler), executor), StateData(std::forward<Args>(args)...) {}
    };

    using state_allocator_type =
        typename std::allocator_traits<completion_handler_allocator_type>::template rebind_alloc<state_holder>;
    using state_allocator_traits = std::allocator_traits<state_allocator_type>;

    struct state_deleter {
        explicit state_deleter(state_allocator_type const &a) : m_allocator(a) {}
        using pointer = typename state_allocator_traits::pointer;
        void operator()(pointer p) const {
            state_allocator_type a(m_allocator);
            state_allocator_traits::destroy(a, std::addressof(*p));
            state_allocator_traits::deallocate(a, p, 1);
        }

      private:
        state_allocator_type m_allocator;
    };
    using state_holder_ptr = std::unique_ptr<state_holder, state_deleter>;

    template <class... Args>
    static state_holder_ptr make_state(completion_handler_type &&completion_handler, Executor const &executor,
                                       Args &&... args);

    state_holder_ptr m_state;
};

template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor, typename StateData>
template <class... Args>
auto async_state<CompletionHandlerSignature, CompletionToken, Executor, StateData>::make_state(
    completion_handler_type &&completion_handler, Executor const &executor, Args &&... args) -> state_holder_ptr {

    static_assert(!std::is_array<StateData>::value);
    state_allocator_type state_allocator{asio::get_associated_allocator(completion_handler, default_allocator{})};
    auto p = state_allocator_traits::allocate(state_allocator, 1);
    bool commit = false;
    BOOST_SCOPE_EXIT_ALL(&) {
        if (!commit) {
            state_allocator_traits::deallocate(state_allocator, p, 1);
            p = nullptr;
        }
    };
    state_allocator_traits::construct(state_allocator, std::addressof(*p), std::move(completion_handler), executor,
                                      std::forward<Args>(args)...);
    BOOST_SCOPE_EXIT_ALL(&) {
        if (!commit) {
            state_allocator_traits::destroy(state_allocator, std::addressof(*p));
        }
    };

    state_holder_ptr state{p, state_deleter(state_allocator)};
    commit = true;
    return state;
}

} // namespace async_utils

#endif
