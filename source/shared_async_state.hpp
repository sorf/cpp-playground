#ifndef SHARED_ASYNC_STATE_HPP
#define SHARED_ASYNC_STATE_HPP

#include <atomic>
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/scope_exit.hpp>
#include <memory>

namespace async_utils {
namespace asio = boost::asio;

// Container of the state associated with multiple asychronous operations.
// It holds the final operation completion handler, an executor to be used as a default if the final completion handler
// doesn't have any associated with it and any other state data used by the implementation of the asychronous operation.
// The memory used to hold the state is managed using the allocator associated with the completion handler.
template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor> class shared_async_state {
  public:
    // Completion type.
    using completion_type = asio::async_completion<CompletionToken, CompletionHandlerSignature>;
    // Completion handler type generated from CompletionHandlerSignature.
    using completion_handler_type = typename completion_type::completion_handler_type;
    // Default allocator if none associated with the completion handler.
    using default_allocator = std::allocator<void>;
    // The allocator associated with the completion handler.
    using completion_handler_allocator_type = asio::associated_allocator_t<completion_handler_type, default_allocator>;

    explicit shared_async_state(completion_handler_type &&completion_handler, Executor &&executor) : m_ptr{} {
        state_allocator_type state_allocator{asio::get_associated_allocator(completion_handler, default_allocator{})};
        auto p = state_allocator_traits::allocate(state_allocator, 1);
        bool commit = false;
        BOOST_SCOPE_EXIT_ALL(&) {
            if (!commit) {
                state_allocator_traits::deallocate(state_allocator, p, 1);
                p = nullptr;
            }
        };
        state_allocator_traits::construct(state_allocator, std::addressof(*p), std::move(completion_handler),
                                          std::move(executor));

        BOOST_SCOPE_EXIT_ALL(&) {
            if (!commit) {
                state_allocator_traits::destroy(state_allocator, std::addressof(*p));
            }
        };

        m_ptr = p;
        commit = true;
    }

    shared_async_state(shared_async_state const &) = delete;
    shared_async_state(shared_async_state &&other) noexcept : m_ptr{} { std::swap(m_ptr, other.m_ptr); }
    shared_async_state &operator=(shared_async_state const &) = delete;
    shared_async_state &operator=(shared_async_state &&other) noexcept {
        reset();
        std::swap(m_ptr, other.m_ptr);
        return *this;
    }
    ~shared_async_state() { reset(); }

    bool has_state() const noexcept { return m_ptr != nullptr; }
    bool unique() const noexcept { return m_ptr != nullptr && m_ptr->ref_count_ == 1; }

    // Invokes the final completion handler with the given arguments.
    // This should be called at most once and only if this is unique().
    // The state is deallocated before calling the handler, so the passsed arguments should not refer any data
    // from this state.
    template <class... Args> void invoke(Args &&... args) {
        BOOST_ASSERT(unique());
        auto handler = release();
        std::invoke(handler, std::forward<Args>(args)...);
    }

    void reset() noexcept {
        if (m_ptr != nullptr) {
            if (--m_ptr->ref_count == 0) {
                release();
            } else {
                m_ptr = nullptr;
            }
        }
    }

  private:
    decltype(auto) release() noexcept {
        BOOST_ASSERT(m_ptr);
        state_allocator_type state_allocator{
            asio::get_associated_allocator(m_ptr->completion_handler, default_allocator{})};
        BOOST_SCOPE_EXIT_ALL(&) {
            state_allocator_traits::destroy(state_allocator, std::addressof(*m_ptr));
            state_allocator_traits::deallocate(state_allocator, m_ptr, 1);
            m_ptr = nullptr;
        };
        return std::move(m_ptr->completion_handler);
    }

    struct state_holder;
    using state_allocator_type =
        typename std::allocator_traits<completion_handler_allocator_type>::template rebind_alloc<state_holder>;
    using state_allocator_traits = std::allocator_traits<state_allocator_type>;

    struct state_holder {
        state_holder(completion_handler_type &&completion_handler, Executor &&executor_arg)
            : completion_handler(std::move(completion_handler)),
              executor(std::move(executor_arg)), io_work{asio::make_work_guard(executor)}, ref_count{1} {}

        state_holder(state_holder const &) = delete;
        state_holder(state_holder &&) = delete;
        state_holder &operator=(state_holder const &) = delete;
        state_holder &operator=(state_holder &&) = delete;
        ~state_holder() = default;

        completion_handler_type completion_handler;
        Executor executor;
        asio::executor_work_guard<Executor> io_work;
        std::atomic_size_t ref_count;
    };
    state_holder *m_ptr;
};

} // namespace async_utils

#endif
