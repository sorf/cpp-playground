#define ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG
#include "async_state.hpp"
#include "bind_allocator.hpp"
#include "stack_handler_allocator.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core/async_op_base.hpp>
#include <boost/format.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;

namespace {

using error_code = boost::system::error_code;

// A composed operations that runs many timer wait operations in series.
template <typename CompletionToken>
auto async_one_timer(asio::io_context &io_context, std::chrono::steady_clock::duration run_duration,
                     CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code)>::return_type {

    using handler_type = typename asio::async_completion<CompletionToken, void(error_code)>::completion_handler_type;
    using base_type = beast::stable_async_op_base<handler_type, asio::io_context::executor_type>;

    struct internal_op : base_type {
        internal_op(asio::io_context &io_context, std::chrono::steady_clock::duration run_duration,
                    handler_type &&handler)
            : base_type{io_context.get_executor(), std::move(handler)}, waits{},
              data{beast::allocate_stable<temporary_data>(*this, io_context, run_duration)} {
        }

        void start_one_wait() {
            ++waits;
            data.internal_timer.expires_after(std::chrono::milliseconds{50});
            data.internal_timer.async_wait(std::move(*this));
        }

        void operator()(error_code ec) {
            if (!ec && std::chrono::steady_clock::now() < data.end_time) {
                std::cout << boost::format("internal_op[waits=%d]: After one wait") % waits << std::endl;
                start_one_wait();
                return;
            }
            // Operation complete here.
            if (!ec) {
                std::cout << boost::format("internal_op[waits=%d]: Done") % waits << std::endl;
            } else {
                std::cout << boost::format("internal_op[waits=%d]: Error: %s:%s") % waits % ec % ec.message()
                          << std::endl;
            }
            this->invoke(ec);
        }

        struct temporary_data {
            temporary_data(asio::io_context &io_context, std::chrono::steady_clock::duration run_duration)
                : internal_timer{io_context}, end_time{std::chrono::steady_clock::now() + run_duration} {}
            asio::steady_timer internal_timer;
            std::chrono::steady_clock::time_point const end_time;
        };

        unsigned waits;
        temporary_data &data;
    };

    typename asio::async_completion<CompletionToken, void(error_code)> completion(token);
    internal_op op{io_context, run_duration, std::move(completion.completion_handler)};
    op.start_one_wait();
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
}