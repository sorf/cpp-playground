//#define ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG
//#define BOOST_ASIO_ENABLE_HANDLER_TRACKING

#include "bind_allocator.hpp"
#include "on_scope_exit.hpp"
#include "shared_async_state.hpp"
#include "stack_handler_allocator.hpp"

#include <atomic>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/format.hpp>
#include <boost/predef.h>
#include <boost/range/irange.hpp>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace asio = boost::asio;

namespace {

using error_code = boost::system::error_code;

// A composed operations that runs many timer wait operations both in parallel and in series.
// The operation receives a user resource (a bool value) that is accessed each time an internal timer wait operation
// completes.This way we test that all the internal operations happen only as part of this user called composed
// operation - no internal operation should execute once the completion callback of the composed operation has been
// called (or scheduled to be called).
//
// The caller is responsible for ensuring that the this call and all the internal operations are running from the same
// implicit or explicit strand.
template <typename CompletionToken>
auto async_many_timers(asio::io_context &io_context, bool &user_resource,
                       std::chrono::steady_clock::duration run_duration, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code)>::return_type {

    struct state_data {
        state_data(asio::io_context &io_context, bool &user_resource)
            : user_resource{user_resource}, run_timer{io_context}, executing{false}, is_open{false} {
            int timer_count = 25;
            internal_timers.reserve(timer_count);
            for (int timer_index = 0; timer_index < timer_count; ++timer_index) {
                internal_timers.emplace_back(io_context);
            }
        }

        state_data(state_data const &) = delete;
        state_data(state_data &&) = delete;
        state_data &operator=(state_data const &) = delete;
        state_data &operator=(state_data &&other) = delete;
        ~state_data() { assert(!executing); }

        bool &user_resource;
        error_code user_completion_error;
        asio::steady_timer run_timer;
        std::vector<asio::steady_timer> internal_timers;
        // Atomic flag to detect if internal completion handlers run in parallel.
        std::atomic_bool executing;
        // If this is not opened anymore, close() has no effect.
        bool is_open;
    };

    using base_type =
        async_utils::shared_async_state<void(error_code), CompletionToken, asio::io_context::executor_type, state_data>;
    struct internal_op : base_type {
        state_data &data;

        internal_op(asio::io_context &io_context, bool &user_resource, std::chrono::steady_clock::duration run_duration,
                    typename base_type::completion_handler_type &&completion_handler)
            : base_type{io_context.get_executor(), std::move(completion_handler), io_context, user_resource},
              data{base_type::get_data()} {

            assert(data.user_resource);
            assert(!data.executing);
            data.executing = true;
            ON_SCOPE_EXIT([&] { data.executing = false; }); // NOTE: This works because we copy this in the lambda below

            // Setting the total run duration.
            data.run_timer.expires_after(run_duration);
            data.run_timer.async_wait(this->wrap([*this](error_code ec) mutable {
                {
                    assert(data.user_resource);
                    assert(!data.executing);
                    data.executing = true;
                    ON_SCOPE_EXIT([&] { data.executing = false; });
                    close(ec);
                }
                try_invoke(); // Note: The executing flag accessed on-scope-exit has to be reset
                              // before we deallocate the state data and call the final handler
            }));

            // Starting the internal timers in parallel.
            for (auto timer_index : boost::irange<std::size_t>(0, data.internal_timers.size())) {
                start_one_wait(timer_index, std::chrono::milliseconds(20 * (timer_index + 1)));
            }
            std::cout << "timers started" << std::endl;
            data.is_open = true;
        }

        void start_one_wait(std::size_t timer_index, std::chrono::steady_clock::duration one_wait) {
            auto &timer = data.internal_timers[timer_index];
            timer.expires_after(one_wait);
            timer.async_wait(this->wrap([=, *this](error_code ec) mutable {
                {
                    assert(data.user_resource);
                    assert(!data.executing);
                    data.executing = true;
                    ON_SCOPE_EXIT([&] { data.executing = false; });

                    if (data.is_open && !ec) {
                        return start_one_wait(timer_index, one_wait);
                    }

                    // Operation complete here.
                    if (!data.is_open) {
                        std::cout << boost::format("timer[%d]: wait closed") % timer_index << std::endl;
                    } else {
                        std::cout << boost::format("timer[%d]: wait error: %s:%s") % timer_index % ec % ec.message()
                                  << std::endl;
                        close(ec);
                    }
                }
                try_invoke(); // Note: Same as above, invoke() after on-scope-exit has run
            }));
        }

        void close(error_code ec) {
            if (!data.is_open) {
                return;
            }
            data.user_completion_error = ec;
            data.is_open = false;
            data.run_timer.cancel();
            for (auto &timer : data.internal_timers) {
                timer.cancel();
            }
            std::cout << "timers cancelled" << std::endl;
        }

        void try_invoke() { this->try_invoke_move_args(data.user_completion_error); }
    };

    typename internal_op::completion_type completion(token);
    internal_op op{io_context, user_resource, run_duration, std::move(completion.completion_handler)};
    return completion.result.get();
}

void test_callback(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    async_utils::stack_handler_memory<32> handler_memory;
    async_utils::stack_handler_allocator<void> handler_allocator(handler_memory);

    auto user_resource = std::make_unique<bool>(true);
    std::cout << "[callback] Timers start" << std::endl;

    async_many_timers(io_context, *user_resource, run_duration,
                      async_utils::bind_allocator(handler_allocator, [&user_resource](error_code const &error) mutable {
                          *user_resource = false;
                          user_resource.reset();

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
    async_utils::stack_handler_memory<32> handler_memory;
    async_utils::stack_handler_allocator<void> handler_allocator(handler_memory);

    auto io_work = asio::make_work_guard(io_context.get_executor());
    std::vector<std::thread> threads;
    int thread_count = 25;
    threads.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&io_context] { io_context.run(); });
    }

    auto user_resource = std::make_unique<bool>(true);
    std::cout << "[callback_strand] Timers start" << std::endl;
    asio::strand<asio::io_context::executor_type> strand_timers(io_context.get_executor());

    // The initiation of the async operation has to be executed in the strand.
    strand_timers.dispatch(
        [&]() {
            async_many_timers(
                io_context, *user_resource, run_duration,
                asio::bind_executor(
                    strand_timers,
                    async_utils::bind_allocator(handler_allocator, [&user_resource](error_code const &error) {
                        *user_resource = false;
                        user_resource.reset();

                        if (error) {
                            std::cout << "[callback_strand] Timers error: " << error.message() << std::endl;
                        } else {
                            std::cout << "[callback_strand] Timers done" << std::endl;
                        }
                    })));
        },
        handler_allocator);

    io_work.reset();
    io_context.run();
    for (auto &t : threads) {
        t.join();
    }
}

void test_future(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    auto user_resource = std::make_unique<bool>(true);
    std::cout << "[future] Timers start" << std::endl;

    std::future<void> f = async_many_timers(io_context, *user_resource, run_duration, asio::use_future);
    std::thread thread_wait_future([f = std::move(f), &user_resource]() mutable {
        try {
            f.get();
            *user_resource = false;
            user_resource.reset();

            std::cout << "[future] Timers done" << std::endl;
        } catch (std::exception const &e) {
            std::cout << "[future] Timers error: " << e.what() << std::endl;
        }
    });
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
    return 0;
}
