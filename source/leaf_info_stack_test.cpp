#include <boost/leaf/all.hpp>
#include <boost/log/utility/unique_identifier_name.hpp>
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

inline decltype(auto) append_estack(std::string_view info, unsigned line) {
    return leaf::accumulate(
        [=](e_stack &) { std::cout << "stack\t<direct>\t" << info << "\t@line\t" << line << std::endl; });
}

template <typename Result> inline void append_estack(Result &r, std::string_view info, unsigned line) {
    r.accumulate([=](e_stack &) { std::cout << "stack\t<reversed>\t" << info << "\t@line\t" << line << std::endl; });
}

// Calls a function under `leaf::capture_in_result` and `leaf::exception_to_result`.
template <typename ErrorHandler, typename R, typename F> leaf::result<R> leaf_call(F &&f) {
    using result_type = leaf::result<R>;
    return leaf::capture_in_result<ErrorHandler>([f = std::forward<F>(f)]() mutable -> result_type {
        return leaf::exception_to_result([&]() -> result_type { return f(); });
    });
}

#define APPEND_ESTACK(info)                                                                                            \
    [[maybe_unused]] auto BOOST_LOG_UNIQUE_IDENTIFIER_NAME(append_estack_0_guard) = ::append_estack(info, __LINE__)

#define APPEND_ESTACK_R(r, info)                                                                                       \
    [[maybe_unused]] auto BOOST_LOG_UNIQUE_IDENTIFIER_NAME(append_estack_1_guard) = ::append_estack(info, __LINE__);   \
    ::append_estack(r, info, __LINE__)

// Operations
// ----------

// op_a:
//          +--asio::post()--+
//          ^                V
//      start                cont1-+
//                                 V
//                             call handler
struct op_a {

    template <typename ErrorHandler, typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        APPEND_ESTACK("op_a::start");
        FAILURE_POINT();
        asio::post(io_context, [h = std::forward<Handler>(h)]() mutable {
            APPEND_ESTACK("op_a::cont1");

            auto r = leaf_call<ErrorHandler, void>([]() -> leaf::result<void> {
                APPEND_ESTACK("op_a::cont2");
                FAILURE_POINT();
                return {};
            });
            h(r);
        });
    }
};

// op_b:
//          +--op_a::start()--+    +--op_a::start()--+
//          ^                V    ^                V
//      start                cont1                 cont3  -+
//                                                         V
//                                                     call handler
struct op_b {

    template <typename ErrorHandler, typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        APPEND_ESTACK("op_b::start");
        FAILURE_POINT();
        op_a::start<ErrorHandler>(io_context, [&, h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            APPEND_ESTACK_R(r, "op_b::cont1");
            if (r) {
                r = leaf_call<ErrorHandler, void>([&]() -> leaf::result<void> {
                    cont_impl<ErrorHandler>(io_context, std::move(h));
                    return {};
                });
            }
            // NOTE: We assume `cont_impl` failed before moving `h`
            if (!r) {
                h(r);
            }
        });
    }

    template <typename ErrorHandler, typename Handler>
    static void cont_impl(asio::io_context &io_context, Handler &&h) {
        APPEND_ESTACK("op_b::cont2");
        FAILURE_POINT();
        op_a::start<ErrorHandler>(io_context, [h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            APPEND_ESTACK_R(r, "op_b::cont3");
            if (r) {
                r = leaf_call<ErrorHandler, void>([]() -> leaf::result<void> {
                    APPEND_ESTACK("op_b::cont4");
                    FAILURE_POINT();
                    return {};
                });
            }
            h(r);
        });
    }
};

// op_c:
//          +--op_b::start()--+    +--op_b::start()--+
//          ^                V    ^                V
//      start                cont1                 cont3  -+
//                                                         V
//                                                     call handler
struct op_c {

    template <typename ErrorHandler, typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        APPEND_ESTACK("op_c::start");
        FAILURE_POINT();
        op_b::start<ErrorHandler>(io_context, [&, h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            APPEND_ESTACK_R(r, "op_c::cont1");
            if (r) {
                r = leaf_call<ErrorHandler, void>([&]() -> leaf::result<void> {
                    cont_impl<ErrorHandler>(io_context, std::move(h));
                    return {};
                });
            }
            // NOTE: We assume `cont_impl` failed before moving `h`
            if (!r) {
                h(r);
            }
        });
    }

    template <typename ErrorHandler, typename Handler>
    static void cont_impl(asio::io_context &io_context, Handler &&h) {
        APPEND_ESTACK("op_c::cont2");
        FAILURE_POINT();
        op_b::start<ErrorHandler>(io_context, [h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            APPEND_ESTACK_R(r, "op_c::cont3");
            if (r) {
                r = leaf_call<ErrorHandler, void>([]() -> leaf::result<void> {
                    APPEND_ESTACK("op_c::cont4");
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

        auto handle_error_impl = [](leaf::verbose_diagnostic_info const &diag, e_stack const *e) {
            std::cout << "Error: any_stack: " << (e != nullptr ? "YES" : "NO") << ", diagnostic:" << diag << std::endl;
        };

        auto error_handler_impl = [&](auto const &error) {
            return leaf::handle_error(
                error,
                [&](std::exception_ptr const &ep, e_stack const *e) {
                    leaf::try_([&] { std::rethrow_exception(ep); },
                               [&](leaf::verbose_diagnostic_info const &diag) { handle_error_impl(diag, e); });
                },
                [&](leaf::verbose_diagnostic_info const &diag, e_stack const *e) { handle_error_impl(diag, e); });
        };

        auto error_handler = [&](leaf::error_in_remote_handle_all const &error) { return error_handler_impl(error); };
        auto try_error_handler = [&](leaf::error_in_remote_try_ const &error) { return error_handler_impl(error); };

        bool retry = true;
        std::size_t start_count = 1;
        while (retry) {
            std::cout << "\n\n----\nRun: " << start_count << std::endl;
            set_failure_counter(start_count++);
            leaf::remote_try_(
                [&] {
                    asio::io_context io_context;
                    APPEND_ESTACK("::main-d");
                    op_c::start<decltype(error_handler)>(io_context, [&](leaf::result<void> r) -> leaf::result<void> {
                        leaf::remote_handle_all(
                            [&]() -> leaf::result<void> {
                                APPEND_ESTACK_R(r, "::main-r");
                                // NOLINTNEXTLINE
                                LEAF_CHECK(r);
                                std::cout << "Success" << std::endl;
                                retry = false;
                                return {};
                            },
                            [&](leaf::error_in_remote_handle_all const &error) { return error_handler(error); });
                        return {};
                    });
                    io_context.run();
                },
                [&](leaf::error_in_remote_try_ const &error) { return try_error_handler(error); });
        }
    } catch (std::exception const &e) {
        std::cout << "Error: " << e.what() << "\n";
    }
    return 0;
}
