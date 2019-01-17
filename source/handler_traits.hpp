#ifndef HANDLER_TRAITS_HPP
#define HANDLER_TRAITS_HPP

#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/assert.hpp>
#include <tuple>

namespace async_utils {
namespace asio = boost::asio;

// Default allocator if none associated with the completion handler.
using default_handler_allocator = std::allocator<void>;

// Completion handler traits.
template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor> struct handler_traits {

    // Executor type.
    using executor_type = Executor;

    // Completion type.
    using completion_type = asio::async_completion<CompletionToken, CompletionHandlerSignature>;

    // Completion handler type
    using handler_type = typename completion_type::completion_handler_type;

    // The allocator associated with a completion handler.
    using allocator_type = asio::associated_allocator_t<handler_type, default_handler_allocator>;

    // The allocator associated with the completion handler rebound for a different type.
    template <typename T>
    using rebound_allocator_type = typename std::allocator_traits<allocator_type>::template rebind_alloc<T>;

    // Returns the allocator associated with the handler rebound for a different type.
    template <typename T> static decltype(auto) get_handler_allocator(handler_type const &handler) {
        rebound_allocator_type<T> allocator = asio::get_associated_allocator(handler, default_handler_allocator{});
        return allocator;
    }

    // The executor associated with the completion handler
    using handler_executor_type = typename asio::associated_executor<handler_type, Executor>::type;

    // Returns the executor associated with the completion handler
    static handler_executor_type get_handler_executor(handler_type const &handler, Executor const &executor) {
        return asio::get_associated_executor(handler, executor);
    }
};

} // namespace async_utils

#endif
