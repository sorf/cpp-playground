#define ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG
#include "bind_allocator.hpp"
#include "stack_handler_allocator.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/format.hpp>
#include <boost/predef.h>
#include <chrono>
#include <compose/stable_transform.hpp>
#include <iostream>
#include <thread>
#include <vector>

namespace asio = boost::asio;

namespace {

using error_code = boost::system::error_code;

// A composed operations that runs many timer wait operations in series.
template <typename CompletionToken>
auto async_one_timer(asio::io_context &io_context, std::chrono::steady_clock::duration run_duration,
                     CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code)>::return_type {

    auto ex = io_context.get_executor();
    typename asio::async_completion<CompletionToken, void(error_code)> completion(token);

    enum initiate_t { initiate };
    struct internal_op {
#if BOOST_COMP_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-local-typedefs"
#endif
        using yield_t = compose::stable_yield_token_t<internal_op, decltype(ex),
                                                      typename decltype(completion)::completion_handler_type>;
#if BOOST_COMP_CLANG
#pragma clang diagnostic pop
#endif

        internal_op(asio::io_context &io_context, std::chrono::steady_clock::duration run_duration)
            : internal_timer{io_context}, end_time{std::chrono::steady_clock::now() + run_duration}, waits{} {}

        compose::upcall_guard start_one_wait(yield_t yield) {
            ++waits;
            internal_timer.expires_after(std::chrono::milliseconds{50});
            return internal_timer.async_wait(yield);
        }

        compose::upcall_guard operator()(yield_t yield, initiate_t /*unused*/) { return start_one_wait(yield); }

        compose::upcall_guard operator()(yield_t yield, error_code ec) {
            if (!ec && std::chrono::steady_clock::now() < end_time) {
                std::cout << boost::format("internal_op[waits=%d]: After one wait") % waits << std::endl;
                return start_one_wait(yield);
            }
            // Operation complete here.
            if (!ec) {
                std::cout << boost::format("internal_op[waits=%d]: Done") % waits << std::endl;
            } else {
                std::cout << boost::format("internal_op[waits=%d]: Error: %s:%s") % waits % ec % ec.message()
                          << std::endl;
            }
            return yield.direct_upcall(ec);
        }

        asio::steady_timer internal_timer;
        std::chrono::steady_clock::time_point const end_time;
        unsigned waits;
    };

    compose::stable_transform(ex, completion, internal_op{io_context, run_duration}).run(initiate);
    return completion.result.get();
}

void test_callback(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    async_utils::stack_handler_memory handler_memory;
    std::cout << "[callback] Timers start" << std::endl;
    async_one_timer(io_context, run_duration,
                    async_utils::bind_allocator(
                        async_utils::stack_handler_allocator<void>(handler_memory), [](error_code const &error) {
                            if (error) {
                                std::cout << "[callback] Timers error: " << error.message() << std::endl;
                            } else {
                                std::cout << "[callback] Timers done" << std::endl;
                            }
                        }));

    io_context.run();
}

void test_future(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    std::cout << "[future] Timers start" << std::endl;

    std::future<void> f = async_one_timer(io_context, run_duration, asio::use_future);
    std::thread thread_wait_future{[f = std::move(f)]() mutable {
        try {
            f.get();
            std::cout << "[future] Timers done" << std::endl;
        } catch (std::exception const &e) {
            std::cout << "[future] Timers error: " << e.what() << std::endl;
        }
    }};
    io_context.run();
    thread_wait_future.join();
}

} // namespace

int main() {
    try {
        test_callback(std::chrono::seconds(1));
        test_future(std::chrono::seconds(1));
    } catch (std::exception const &e) {
        std::cout << "Error: " << e.what() << "\n";
    }
    return 0;
}