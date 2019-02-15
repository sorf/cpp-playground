#include <boost/leaf/all.hpp>
#include <boost/log/utility/unique_identifier_name.hpp>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <string_view>

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
    std::list<std::pair<std::string_view, unsigned>> value;
    friend std::ostream &operator<<(std::ostream &os, e_stack const &s) {
        os << "e_stack: ";
        for (auto &i : s.value) {
            os << '(' << i.first << ',' << i.second << "), ";
        }
        return os;
    }
};

inline decltype(auto) append_estack(std::string_view info, unsigned line) {
    return leaf::accumulate([=](e_stack &e) { e.value.emplace_front(info, line); });
}

template <typename Result> inline void append_estack(Result &r, std::string_view info, unsigned line) {
    r.accumulate([=](e_stack &e) { e.value.emplace_back(info, line); });
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

    template <typename ErrorContext, typename Handler>
    static void start(asio::io_context &io_context, ErrorContext &error_context, Handler &&h) {
        APPEND_ESTACK("op_a::start");
        FAILURE_POINT();
        asio::post(io_context, [&, h = std::forward<Handler>(h)]() mutable {
            leaf::result<void> r;
            {
                leaf::context_activator active_context(error_context,
                                                       leaf::context_activator::on_deactivation::do_not_propagate);
                APPEND_ESTACK("op_a::cont1");
                r = leaf::exception_to_result([&]() -> leaf::result<void> {
                    APPEND_ESTACK("op_a::cont2");
                    FAILURE_POINT();
                    return {};
                });
            }
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

    template <typename ErrorContext, typename Handler>
    static void start(asio::io_context &io_context, ErrorContext &error_context, Handler &&h) {
        APPEND_ESTACK("op_b::start");
        FAILURE_POINT();
        op_a::start(io_context, error_context, [&, h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            {
                leaf::context_activator active_context(error_context,
                                                       leaf::context_activator::on_deactivation::do_not_propagate);
                APPEND_ESTACK_R(r, "op_b::cont1");
                if (r) {
                    r = leaf::exception_to_result([&]() -> leaf::result<void> {
                        cont_impl(io_context, error_context, std::move(h));
                        return {};
                    });
                }
            }
            // NOTE: We assume `cont_impl` failed before moving `h`
            if (!r) {
                h(r);
            }
        });
    }

    template <typename ErrorContext, typename Handler>
    static void cont_impl(asio::io_context &io_context, ErrorContext &error_context, Handler &&h) {
        APPEND_ESTACK("op_b::cont2");
        FAILURE_POINT();
        op_a::start(io_context, error_context, [&, h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            {
                leaf::context_activator active_context(error_context,
                                                       leaf::context_activator::on_deactivation::do_not_propagate);
                APPEND_ESTACK_R(r, "op_b::cont3");
                if (r) {
                    r = leaf::exception_to_result([&]() -> leaf::result<void> {
                        APPEND_ESTACK("op_b::cont4");
                        FAILURE_POINT();
                        return {};
                    });
                }
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

    template <typename ErrorContext, typename Handler>
    static void start(asio::io_context &io_context, ErrorContext &error_context, Handler &&h) {
        APPEND_ESTACK("op_c::start");
        FAILURE_POINT();
        op_b::start(io_context, error_context, [&, h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            {
                leaf::context_activator active_context(error_context,
                                                       leaf::context_activator::on_deactivation::do_not_propagate);
                APPEND_ESTACK_R(r, "op_c::cont1");
                if (r) {
                    r = leaf::exception_to_result([&]() -> leaf::result<void> {
                        cont_impl(io_context, error_context, std::move(h));
                        return {};
                    });
                }
            }
            // NOTE: We assume `cont_impl` failed before moving `h`
            if (!r) {
                h(r);
            }
        });
    }

    template <typename ErrorContext, typename Handler>
    static void cont_impl(asio::io_context &io_context, ErrorContext &error_context, Handler &&h) {
        APPEND_ESTACK("op_c::cont2");
        FAILURE_POINT();
        op_b::start(io_context, error_context, [&, h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
            {
                leaf::context_activator active_context(error_context,
                                                       leaf::context_activator::on_deactivation::do_not_propagate);
                APPEND_ESTACK_R(r, "op_c::cont3");
                if (r) {
                    r = leaf::exception_to_result([&]() -> leaf::result<void> {
                        APPEND_ESTACK("op_c::cont4");
                        FAILURE_POINT();
                        return {};
                    });
                }
            }
            h(r);
        });
    }
};

template <class TryBlock, class RemoteH>
typename std::decay<decltype(std::declval<TryBlock>()())>::type leaf_remote_try_catch_no_lint(TryBlock &&try_block,
                                                                                              RemoteH &&h) {
#ifndef __clang_analyzer__
    return leaf::remote_try_catch(std::forward<TryBlock>(try_block), std::forward<RemoteH>(h));
#else
    (void)h;
    return try_block();
#endif
}

template <class Context, class R, class RemoteH>
R leaf_remote_context_handle_some(Context &context, R const &r, RemoteH &&h) {
#ifndef __clang_analyzer__
    return context.remote_handle_some(r, std::forward<RemoteH>(h));
#else
    (void)context;
    (void)h;
    return r;
#endif
}

int main() {
    try {

        auto handle_error_impl = [&](leaf::verbose_diagnostic_info const &diag,
                                     e_stack const *e) -> leaf::result<void> {
            std::cout << "Error: \n    any_stack: " << (e != nullptr ? "YES" : "NO");
            if (e != nullptr) {
                std::cout << "\n";
                for (auto const &s : e->value) {
                    std::cout << "        " << s.first << "\t@line\t" << s.second << std::endl;
                }
            }
            std::cout << "    diagnostic:" << diag << std::endl;
            // Note: This cannot be enabled yet as the LEAF context.remote_handle_some() calling this error handling
            // functionality is marked as `noexcept`.
            // throw std::runtime_error("error handling failure");
            return {};
        };

        auto error_handler = [&](leaf::error_info const &error) {
            return leaf::remote_handle_some(
                error,
                [&](std::exception_ptr const &ep, e_stack const *e) {
                    return leaf::try_catch(
                        [&]() -> leaf::result<void> { std::rethrow_exception(ep); },
                        [&](leaf::verbose_diagnostic_info const &diag) { return handle_error_impl(diag, e); });
                },
                [&](leaf::verbose_diagnostic_info const &diag, e_stack const *e) {
                    return handle_error_impl(diag, e);
                });
        };

        bool retry = true;
        std::size_t start_count = 1;
        while (retry) {
            std::cout << "\n----\nRun: " << start_count << std::endl;
            set_failure_counter(start_count++);

            leaf_remote_try_catch_no_lint(
                [&]() -> leaf::result<void> {
                    asio::io_context io_context;
                    auto error_context = leaf::make_context(&error_handler);

                    APPEND_ESTACK("::main-d");
                    op_c::start(io_context, error_context, [&](leaf::result<void> r) {
                        leaf::context_activator active_context(
                            error_context, leaf::context_activator::on_deactivation::do_not_propagate);

                        APPEND_ESTACK_R(r, "::main-r");
                        if (r) {
                            std::cout << "Success" << std::endl;
                            retry = false;
                        } else {
                            [[maybe_unused]] auto rr = leaf_remote_context_handle_some(
                                error_context, r, [&](leaf::error_info const &error) { return error_handler(error); });
                        }
                    });
                    io_context.run();
                    return {};
                },
                [&](leaf::error_info const &error) { return error_handler(error); });
        }
    } catch (std::exception const &e) {
        std::cout << "Fatal Error: " << e.what() << "\n";
    }
    return 0;
}