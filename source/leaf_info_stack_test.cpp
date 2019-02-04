#include <boost/leaf/all.hpp>
#include <functional>
#include <future>
#include <iostream>
#include <list>

// Asio run loop
// -------------
namespace asio_simulator {

struct io_context {

    void run() {
        while (!execution_queue.empty()) {
            auto e = std::move(execution_queue.back());
            execution_queue.pop_back();
            e();
        }
    }

    std::list<std::function<void()>> execution_queue;
};

template <typename F> void post(io_context &io_context, F &&f) {
    io_context.execution_queue.emplace_back(std::forward<F>(f));
}

} // namespace asio_simulator

namespace asio = asio_simulator;
namespace leaf = boost::leaf;

// Failure generation
// ------------------

std::size_t &failure_counter() {
    static std::size_t counter = 0;
    return counter;
}

void set_failure_counter(std::size_t start_count) { failure_counter() = start_count; }

void failure_point(int line) {
    std::size_t &counter = failure_counter();

    if (counter != 0) {
        --counter;
    }
    if (counter == 0) {
        throw std::runtime_error("error at line: " + std::to_string(line));
    }
}

#define FAILURE_POINT() failure_point(__LINE__)

// Error Handlers
// --------------

struct e_stack {
    std::nullptr_t value;
};

auto g_handle_error_impl = [](leaf::verbose_diagnostic_info const &diag, e_stack const *e) {
    std::cout << "Error: any_stack: " << (e != nullptr ? "YES" : "NO") << ", diagnostic:" << diag << std::endl;
};

auto g_try_error_handler = [&](leaf::error_in_remote_try_ const &error) {
    return leaf::handle_error(
        error, [&](leaf::verbose_diagnostic_info const &diag, e_stack const *e) { g_handle_error_impl(diag, e); });
};

auto g_all_error_handler = [&](leaf::error_in_remote_handle_all const &error) {
    return leaf::handle_error(
        error,
        [&](std::exception_ptr const &ep, e_stack const *e) {
            leaf::try_([&] { std::rethrow_exception(ep); },
                       [&](leaf::verbose_diagnostic_info const &diag) { g_handle_error_impl(diag, e); });
        },
        [&](leaf::verbose_diagnostic_info const &diag, e_stack const *e) { g_handle_error_impl(diag, e); });
};

using all_error_handler_type = decltype(g_all_error_handler);

// Calls a function under `leaf::capture_in_result` and `leaf::exception_to_result`.
template <typename R, typename F> leaf::result<R> leaf_call(F &&f) {
    using result_type = leaf::result<R>;
    return leaf::capture_in_result<all_error_handler_type>([f = std::forward<F>(f)]() mutable -> result_type {
        return leaf::exception_to_result([&]() -> result_type { return f(); });
    });
}

// Operations
// ----------

// opA:
//          +--asio::post()--+
//          ^                V
//      start                cont1-+
//                                 V
//                             call handler
struct opA {

    template <typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto acc = leaf::accumulate([](e_stack &) { std::cout << "stack: opA::start\n"; });
        FAILURE_POINT();
        asio::post(io_context, [h = std::forward<Handler>(h)]() mutable {
            [[maybe_unused]] auto acc1 = leaf::accumulate([](e_stack &) { std::cout << "stack: opA::cont1\n"; });

            auto r = leaf_call<void>([]() -> leaf::result<void> {
                [[maybe_unused]] auto acc2 = leaf::accumulate([](e_stack &) { std::cout << "stack: opA::cont2\n"; });
                FAILURE_POINT();
                return {};
            });
            h(r);
        });
    }
};

// opB:
//          +--opA::start()--+    +--opA::start()--+
//          ^                V    ^                V
//      start                cont1                 cont3  -+
//                                                         V
//                                                     call handler
struct opB {

    template <typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto acc = leaf::accumulate([](e_stack &) { std::cout << "stack: opB::start\n"; });
        FAILURE_POINT();
        opA::start(io_context, [&, h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            r.accumulate([](e_stack &) { std::cout << "stack: opB::cont1-r\n"; });
            [[maybe_unused]] auto acc1 = leaf::accumulate([](e_stack &) { std::cout << "stack: opB::cont1-a\n"; });
            if (r) {
                r = leaf_call<void>([&]() -> leaf::result<void> {
                    cont_impl(io_context, std::move(h));
                    return {};
                });
            }
            // NOTE: We assume `cont_impl` failed before moving `h`
            if (!r) {
                h(r);
            }
        });
    }

    template <typename Handler> static void cont_impl(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto acc2 = leaf::accumulate([](e_stack &) { std::cout << "stack: opB::cont2\n"; });
        FAILURE_POINT();
        opA::start(io_context, [h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            r.accumulate([](e_stack &) { std::cout << "stack: opB::cont3-r\n"; });
            [[maybe_unused]] auto acc3 = leaf::accumulate([](e_stack &) { std::cout << "stack: opB::cont3-a\n"; });
            if (r) {
                r = leaf_call<void>([]() -> leaf::result<void> {
                    [[maybe_unused]] auto acc4 =
                        leaf::accumulate([](e_stack &) { std::cout << "stack: opB::cont4\n"; });
                    FAILURE_POINT();
                    return {};
                });
            }
            h(r);
        });
    }
};

// opC:
//          +--opB::start()--+    +--opB::start()--+
//          ^                V    ^                V
//      start                cont1                 cont3  -+
//                                                         V
//                                                     call handler
struct opC {

    template <typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto acc = leaf::accumulate([](e_stack &) { std::cout << "stack: opC::start\n"; });
        FAILURE_POINT();
        opB::start(io_context, [&, h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            r.accumulate([](e_stack &) { std::cout << "stack: opC::cont1-r\n"; });
            [[maybe_unused]] auto acc1 = leaf::accumulate([](e_stack &) { std::cout << "stack: opC::cont1-a\n"; });
            if (r) {
                r = leaf_call<void>([&]() -> leaf::result<void> {
                    cont_impl(io_context, std::move(h));
                    return {};
                });
            }
            // NOTE: We assume `cont_impl` failed before moving `h`
            if (!r) {
                h(r);
            }
        });
    }

    template <typename Handler> static void cont_impl(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto acc2 = leaf::accumulate([](e_stack &) { std::cout << "stack: opC::cont2\n"; });
        FAILURE_POINT();
        opB::start(io_context, [h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            r.accumulate([](e_stack &) { std::cout << "stack: opC::cont3-r\n"; });
            [[maybe_unused]] auto acc3 = leaf::accumulate([](e_stack &) { std::cout << "stack: opC::cont3-a\n"; });
            if (r) {
                r = leaf_call<void>([]() -> leaf::result<void> {
                    [[maybe_unused]] auto acc4 =
                        leaf::accumulate([](e_stack &) { std::cout << "stack: opC::cont4\n"; });
                    FAILURE_POINT();
                    return {};
                });
            }
            h(r);
        });
    }
};

int main() {
    try {
        bool retry = true;
        std::size_t start_count = 1;
        while (retry) {
            std::cout << "Run: " << start_count << std::endl;
            set_failure_counter(start_count++);
            try {
                asio::io_context io_context;
                leaf::remote_try_(
                    [&] {
                        [[maybe_unused]] auto acc = leaf::accumulate([](e_stack &) { std::cout << "stack: main\n"; });
                        opC::start(io_context, [&](leaf::result<void> r) -> leaf::result<void> {
                            leaf::remote_handle_all(
                                [&]() -> leaf::result<void> {
                                    // NOLINTNEXTLINE
                                    LEAF_CHECK(r);
                                    std::cout << "Success" << std::endl;
                                    retry = false;
                                    return {};
                                },
                                [&](leaf::error_in_remote_handle_all const &error) {
                                    return g_all_error_handler(error);
                                });
                            return {};
                        });
                    },
                    [&](leaf::error_in_remote_try_ const &error) { return g_try_error_handler(error); });

                io_context.run();
            } catch (std::exception const &e) {
                std::cout << "Run-Error: " << e.what() << "\n";
            }
        }
    } catch (std::exception const &e) {
        std::cout << "Error: " << e.what() << "\n";
    }
    return 0;
}
