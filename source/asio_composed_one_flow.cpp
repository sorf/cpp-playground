#define ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG
#include "bind_allocator.hpp"
#include "shared_async_state.hpp"
#include "stack_handler_allocator.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/format.hpp>
#include <chrono>
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

    struct state_data {
        state_data(asio::io_context &io_context, std::chrono::steady_clock::duration run_duration)
            : internal_timer{io_context}, end_time{std::chrono::steady_clock::now() + run_duration}, waits{} {}
        asio::steady_timer internal_timer;
        std::chrono::steady_clock::time_point const end_time;
        unsigned waits;
    };

    using base_type =
        async_utils::shared_async_state<void(error_code), CompletionToken, asio::io_context::executor_type, state_data>;
    struct internal_op : base_type {
        using base_type::invoke; // MSVC workaround ('this->invoke()` fails to compile in the lambda below)
        state_data &data;

        internal_op(asio::io_context &io_context, std::chrono::steady_clock::duration run_duration,
                    typename base_type::handler_type &&completion_handler)
            : base_type{io_context.get_executor(), std::move(completion_handler), io_context, run_duration},
              data{base_type::get_data()} {
            start_one_wait();
        }

        void start_one_wait() {
            ++data.waits;
            data.internal_timer.expires_after(std::chrono::milliseconds{50});
            data.internal_timer.async_wait(this->wrap([*this](error_code ec) mutable {
                if (!ec && std::chrono::steady_clock::now() < data.end_time) {
                    std::cout << boost::format("internal_op[waits=%d]: After one wait") % data.waits << std::endl;
                    return start_one_wait();
                }
                // Operation complete here.
                if (!ec) {
                    std::cout << boost::format("internal_op[waits=%d]: Done") % data.waits << std::endl;
                } else {
                    std::cout << boost::format("internal_op[waits=%d]: Error: %s:%s") % data.waits % ec % ec.message()
                              << std::endl;
                }
                invoke(ec);
            }));
        }
    };

    typename internal_op::completion_type completion(token);
    internal_op op{io_context, run_duration, std::move(completion.completion_handler)};
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
