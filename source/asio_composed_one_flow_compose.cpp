#include "async_state.hpp"
#include "bind_allocator.hpp"
#include "stack_handler_allocator.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/format.hpp>
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
    auto op = [internal_timer = asio::steady_timer{io_context},
               end_time = std::chrono::steady_clock::now() + run_duration,
               waits = 0](auto yield, error_code ec = {}) mutable {
        if (!yield.is_continuation() || (!ec && std::chrono::steady_clock::now() < end_time)) {
            if (yield.is_continuation()) {
                std::cout << boost::format("internal_op[waits=%d]: After one wait") % waits << std::endl;
            }
            ++waits;
            internal_timer.expires_after(std::chrono::milliseconds{50});
            return internal_timer.async_wait(yield);
        }

        // Operation complete here.
        if (!ec) {
            std::cout << boost::format("internal_op[waits=%d]: Done") % waits << std::endl;
        } else {
            std::cout << boost::format("internal_op[waits=%d]: Error: %s:%s") % waits % ec % ec.message() << std::endl;
        }
        return yield.direct_upcall(ec);
    };

    typename asio::async_completion<CompletionToken, void(error_code)> completion(token);
    compose::stable_transform(ex, completion, std::move(op)).run();
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