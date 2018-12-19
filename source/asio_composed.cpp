#include <atomic>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/io_context_strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/format.hpp>
#include <boost/predef.h>
#include <boost/range/irange.hpp>
#include <boost/scope_exit.hpp>
#include <cassert>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <random>
#include <thread>

namespace asio = boost::asio;
using error_code = boost::system::error_code;

// A composed operations that runs many timer wait operations both in parallel and in series.
// The operation receives a user resource (a bool value) that is accessed each time an internal timer wait operation
// completes.This way we test that all the internal operations happen only as part of this user called composed
// operation - no internal operation should execute once the completion callback of the composed operation has been
// called (or scheduled to be called).
template <typename CompletionToken>
auto async_many_timers(asio::io_context &io_context, bool &user_resource,
                       std::chrono::steady_clock::duration run_duration, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code)>::return_type {

    using completion_handler_sig = void(error_code);
    using completion_type = asio::async_completion<CompletionToken, completion_handler_sig>;
    using completion_handler_type = typename completion_type::completion_handler_type;

    struct internal_state : public std::enable_shared_from_this<internal_state> {
        internal_state(asio::io_context &io_context, bool &user_resource,
                       completion_handler_type user_completion_handler)
            : io_context{io_context}, user_resource{user_resource},
              user_completion_handler{std::move(user_completion_handler)}, io_work{asio::make_work_guard(
                                                                               io_context.get_executor())},
              run_timer{io_context}, is_open{false}, executing{false} {

            int timer_count = 25;
            internal_timers.reserve(timer_count);
            for (int timer_index = 0; timer_index < timer_count; ++timer_index) {
                internal_timers.emplace_back(io_context);
            }
        }

        internal_state(internal_state const &) = delete;
        internal_state(internal_state &&) = delete;
        internal_state &operator=(internal_state const &) = delete;
        internal_state &operator=(internal_state &&) = delete;

        ~internal_state() {
            assert(!executing);
            executing = true;
            BOOST_SCOPE_EXIT_ALL(&) { executing = false; };

            // Only when the destructor of the internal state is reached, we can conclude that all the internal
            // operations running in parallel have completed.
            io_work.reset();
            // We call the handler, and in case it throws, we post the throwing of the exception in io_service::run().
            try {
                user_completion_handler(user_completion_error);
            } catch (...) {
                try {
                    io_context.post(asio::bind_executor(get_executor(),
                                                        [e = std::current_exception()] { std::rethrow_exception(e); }));
                } catch (...) {
                    // Not much we can do here if post() failed.
                }
            }
        }

        void start_many_waits(std::chrono::steady_clock::duration run_duration) {
            assert(user_resource);
            assert(!executing);
            executing = true;
            BOOST_SCOPE_EXIT_ALL(&) { executing = false; };

            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<std::size_t> one_wait_distribution(1, internal_timers.size());

            // Setting the total run duration.
            run_timer.expires_after(run_duration);
            run_timer.async_wait(
                asio::bind_executor(get_executor(), [this, self = this->shared_from_this()](error_code ec) {
                    assert(!executing);
                    executing = true;
                    BOOST_SCOPE_EXIT_ALL(&) { executing = false; };

                    close(ec);
                }));

            // Starting the internal timers in parallel.
            for (auto timer_index : boost::irange<std::size_t>(0, internal_timers.size())) {
                start_one_wait(timer_index, std::chrono::milliseconds(one_wait_distribution(gen)));
            }
            std::cout << "timers started" << std::endl;
            is_open = true;
        }

        void close(error_code ec) {
            if (!is_open) {
                return;
            }
            user_completion_error = ec;
            is_open = false;
            run_timer.cancel();
            for (auto &timer : internal_timers) {
                timer.cancel();
            }
            std::cout << "timers cancelled" << std::endl;
        }

        void start_one_wait(std::size_t timer_index, std::chrono::steady_clock::duration one_wait) {
            auto &timer = internal_timers[timer_index];
            timer.expires_after(one_wait);
            timer.async_wait(asio::bind_executor(
                get_executor(), [this, timer_index, one_wait, self = this->shared_from_this()](error_code ec) {
                    assert(user_resource);
                    assert(!executing);
                    executing = true;
                    BOOST_SCOPE_EXIT_ALL(&) { executing = false; };

                    if (is_open) {
                        start_one_wait(timer_index, one_wait);
                    } else if (!is_open) {
                        std::cout << boost::format("timer[%d]: wait closed") % timer_index << std::endl;
                    } else {
                        std::cout << boost::format("timer[%d]: wait error: %s:%s") % ec % ec.message() << std::endl;
                        close(ec);
                    }
                }));
        }

#if BOOST_COMP_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-local-typedefs"
#endif
        using executor_type = asio::associated_executor_t<completion_handler_type, asio::io_context::executor_type>;
#if BOOST_COMP_CLANG
#pragma clang diagnostic pop
#endif
        executor_type get_executor() const noexcept {
            return asio::get_associated_executor(user_completion_handler, io_context.get_executor());
        }

        asio::io_context &io_context;
        bool &user_resource;
        completion_handler_type user_completion_handler;
        error_code user_completion_error;
        asio::executor_work_guard<asio::io_context::executor_type> io_work;

        asio::steady_timer run_timer;
        std::vector<asio::steady_timer> internal_timers;
        bool is_open;
        std::atomic_bool executing;
    };

    completion_type completion(token);
    std::make_shared<internal_state>(io_context, user_resource, std::move(completion.completion_handler))
        ->start_many_waits(run_duration);
    return completion.result.get();
}

void test_callback(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    auto user_resource = std::make_unique<bool>(true);
    std::cout << "[callback] Timers start" << std::endl;
    async_many_timers(io_context, *user_resource, run_duration, [&user_resource](error_code const &error) {
        *user_resource = false;
        user_resource.reset();

        if (error) {
            std::cout << "[callback] Timers error: " << error.message() << std::endl;
        } else {
            std::cout << "[callback] Timers done" << std::endl;
        }
    });

    io_context.run();
}

void test_callback_strand(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    auto io_work = asio::make_work_guard(io_context.get_executor());
    std::vector<std::thread> threads;
    int thread_count = 25;
    threads.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&io_context] { io_context.run(); });
    }

    auto user_resource = std::make_unique<bool>(true);
    std::cout << "[callback_strand] Timers start" << std::endl;
    asio::io_context::strand strand_timers(io_context);
    async_many_timers(io_context, *user_resource, run_duration,
                      asio::bind_executor(strand_timers, [&user_resource](error_code const &error) {
                          *user_resource = false;
                          user_resource.reset();

                          if (error) {
                              std::cout << "[callback_strand] Timers error: " << error.message() << std::endl;
                          } else {
                              std::cout << "[callback_strand] Timers done" << std::endl;
                          }
                      }));

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

void test_future_strand(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    auto io_work = asio::make_work_guard(io_context.get_executor());
    std::vector<std::thread> threads;
    int thread_count = 25;
    threads.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&io_context] { io_context.run(); });
    }

    auto user_resource = std::make_unique<bool>(true);
    std::cout << "[future_strand] Timers start" << std::endl;
    asio::io_context::strand strand_timers(io_context);
    std::future<void> f = async_many_timers(io_context, *user_resource, run_duration, asio::use_future);
    //                                      TODO(sorf):    How to add strand_timers here ^^^ ?
    try {
        f.get();
        *user_resource = false;
        user_resource.reset();

        std::cout << "[future_strand] Timers done" << std::endl;
    } catch (std::exception const &e) {
        std::cout << "[future_strand] Timers error: " << e.what() << std::endl;
    }

    io_work.reset();
    for (auto &t : threads) {
        t.join();
    }
}
int main() {
    try {
        test_callback(std::chrono::seconds(1));
        test_callback_strand(std::chrono::seconds(1));
        test_future(std::chrono::seconds(1));
        // Uncomment to run the strand + future test.
        // Currently it fails (asserts) as internal operations of the composed operation get to be executed in parallel
        // in different threads.
#if 0
        test_future_strand(std::chrono::seconds(1));
#endif
    } catch (std::exception const &e) {
        std::cout << "Error: " << e.what() << "\n";
    }
}
