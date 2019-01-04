#include "async_state.hpp"
#include "bind_allocator.hpp"
#include "stack_handler_allocator.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
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

    struct internal_op {
        using state_type =
            async_utils::async_state<void(error_code), CompletionToken, asio::io_context::executor_type, internal_op>;

        internal_op(asio::io_context &io_context, std::chrono::steady_clock::duration run_duration)
            : internal_timer{io_context}, end_time{std::chrono::steady_clock::now() + run_duration}, waits{} {}

        void start_one_wait(state_type &&state) {
            internal_timer.expires_after(std::chrono::milliseconds{50});
            internal_timer.async_wait(state.wrap()([this, state = std::move(state)](error_code ec) mutable {
                ++waits;
                if (!ec && std::chrono::steady_clock::now() < end_time) {
                    std::cout << boost::format("internal_op[waits=%d]: After one wait") % waits << std::endl;
                    start_one_wait(std::move(state));
                    return;
                }
                // Operation complete here.
                if (!ec) {
                    std::cout << boost::format("internal_op[waits=%d]: Done") % waits << std::endl;
                } else {
                    std::cout << boost::format("internal_op[waits=%d]: Error: %s:%s") % waits % ec % ec.message()
                              << std::endl;
                }
                state.invoke(ec);
            }));
        }

        asio::steady_timer internal_timer;
        std::chrono::steady_clock::time_point const end_time;
        unsigned waits;
    };

    typename internal_op::state_type::completion_type completion(token);
    typename internal_op::state_type state{std::move(completion.completion_handler), io_context.get_executor(),
                                           io_context, run_duration};
    auto *op = state.get();
    op->start_one_wait(std::move(state));
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

void test_callback_strand(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    async_utils::stack_handler_memory handler_memory;
    auto io_work = asio::make_work_guard(io_context.get_executor());
    std::vector<std::thread> threads;
    int thread_count = 25;
    threads.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&io_context] { io_context.run(); });
    }

    std::cout << "[callback_strand] Timers start" << std::endl;
    asio::strand<asio::io_context::executor_type> strand_timers(io_context.get_executor());

    async_one_timer(
        io_context, run_duration,
        asio::bind_executor(
            strand_timers, async_utils::bind_allocator(
                               async_utils::stack_handler_allocator<void>(handler_memory), [](error_code const &error) {
                                   if (error) {
                                       std::cout << "[callback_strand] Timers error: " << error.message() << std::endl;
                                   } else {
                                       std::cout << "[callback_strand] Timers done" << std::endl;
                                   }
                               })));

    io_work.reset();
    io_context.run();
    for (auto &t : threads) {
        t.join();
    }
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
        test_callback_strand(std::chrono::seconds(1));
        test_future(std::chrono::seconds(1));
    } catch (std::exception const &e) {
        std::cout << "Error: " << e.what() << "\n";
    }
}
