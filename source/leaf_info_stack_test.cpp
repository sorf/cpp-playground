#include "bind_ehandlers_type.hpp"
#include "leaf_call.hpp"

#include <boost/leaf/all.hpp>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <functional>
#include <future>
#include <iostream>
#include <list>

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

template <typename F>
void post(io_context &io_context, F&& f) {
    io_context.execution_queue.emplace_back(std::forward<F>(f));
}

} // namespace asio_simulator

namespace asio = asio_simulator;
namespace leaf = boost::leaf;

struct e_stack {
    std::nullptr_t value;
};

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

// opA:
//          +--asio::post()--+
//          ^                V
//      start                cont -+
//                                 V
//                             call handler
struct opA {

    template <typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto acc = leaf::accumulate([](e_stack &) { std::cout << "stack: opA::start\n"; });
        FAILURE_POINT();
        asio::post(io_context, async_utils::bind_ehandlers_type_from<Handler>([h = std::forward<Handler>(h)]() mutable {
                       using error_handler_type = async_utils::associated_ehandlers_type_t<Handler>;
                       auto r = async_utils::leaf_call<void, error_handler_type>([]() -> leaf::result<void> {
                           [[maybe_unused]] auto acc =
                               leaf::accumulate([](e_stack &) { std::cout << "stack: opA::cont\n"; });
                           FAILURE_POINT();
                           return {};
                       });
                       h(r);
                   }));
    }
};

// opB:
//          +--opA::start()--+    +--opA::start()--+
//          ^                V    ^                V
//      start                cont_1                cont_2 -+
//                                                         V
//                                                     call handler
struct opB {

    template <typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto acc = leaf::accumulate([](e_stack &) { std::cout << "stack: opB::start\n"; });
        FAILURE_POINT();
        opA::start(io_context, async_utils::bind_ehandlers_type_from<Handler>([&, h = std::forward<Handler>(h)](
                                                                                  leaf::result<void> r) mutable {
                       if (r) {
                           using error_handler_type = async_utils::associated_ehandlers_type_t<Handler>;
                           r = async_utils::leaf_call<void, error_handler_type>([&]() -> leaf::result<void> {
                               cont_1(io_context, std::move(h));
                               return {};
                           });
                       }
                       if (!r) {
                           h(r); // NOTE: We assume `cont_1` failed before moving `h`
                       }
                   }));
    }

    template <typename Handler> static void cont_1(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto acc = leaf::accumulate([](e_stack &) { std::cout << "stack: opB::cont_1\n"; });
        FAILURE_POINT();
        opA::start(io_context, async_utils::bind_ehandlers_type_from<Handler>([h = std::forward<Handler>(h)](
                                                                                  leaf::result<void> r) mutable {
                       if (r) {
                           using error_handler_type = async_utils::associated_ehandlers_type_t<Handler>;
                           r = async_utils::leaf_call<void, error_handler_type>([]() -> leaf::result<void> {
                               [[maybe_unused]] auto acc =
                                   leaf::accumulate([](e_stack &) { std::cout << "stack: opB::cont_2\n"; });
                               FAILURE_POINT();
                               return {};
                           });
                       }
                       h(r);
                   }));
    }
};

// opC:
//          +--opB::start()--+    +--opB::start()--+
//          ^                V    ^                V
//      start                cont_1                cont_2 -+
//                                                         V
//                                                     call handler
struct opC {

    template <typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto acc = leaf::accumulate([](e_stack &) { std::cout << "stack: opC::start\n"; });
        FAILURE_POINT();
        opB::start(io_context, async_utils::bind_ehandlers_type_from<Handler>([&, h = std::forward<Handler>(h)](
                                                                                  leaf::result<void> r) mutable {
                       if (r) {
                           using error_handler_type = async_utils::associated_ehandlers_type_t<Handler>;
                           r = async_utils::leaf_call<void, error_handler_type>([&]() -> leaf::result<void> {
                               cont_1(io_context, std::move(h));
                               return {};
                           });
                       }
                       if (!r) {
                           h(r); // NOTE: We assume `cont_1` failed before moving `h`
                       }
                   }));
    }

    template <typename Handler> static void cont_1(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto acc = leaf::accumulate([](e_stack &) { std::cout << "stack: opC::cont_1\n"; });
        FAILURE_POINT();
        opB::start(io_context, async_utils::bind_ehandlers_type_from<Handler>([h = std::forward<Handler>(h)](
                                                                                  leaf::result<void> r) mutable {
                       if (r) {
                           using error_handler_type = async_utils::associated_ehandlers_type_t<Handler>;
                           r = async_utils::leaf_call<void, error_handler_type>([]() -> leaf::result<void> {
                               [[maybe_unused]] auto acc =
                                   leaf::accumulate([](e_stack &) { std::cout << "stack: opC::cont_2\n"; });
                               FAILURE_POINT();
                               return {};
                           });
                       }
                       h(r);
                   }));
    }
};

int main() {
    try {
        auto handle_error_impl = [](leaf::verbose_diagnostic_info const &diag, e_stack const *e) {
            std::cout << "Error: any_stack: " << (e != nullptr ? "YES" : "NO") << ", diagnostic:" << diag << std::endl;
        };

        auto try_error_handler = [&](leaf::error_in_remote_try_ const &error) {
            return leaf::handle_error(error, [&](leaf::verbose_diagnostic_info const &diag, e_stack const *e) {
                handle_error_impl(diag, e);
            });
        };

        auto all_error_handler = [&](leaf::error_in_remote_handle_all const &error) {
            return leaf::handle_error(
                error,
                [&](std::exception_ptr const &ep, e_stack const *e) {
                    leaf::try_([&] { std::rethrow_exception(ep); },
                               [&](leaf::verbose_diagnostic_info const &diag) { handle_error_impl(diag, e); });
                },
                [&](leaf::verbose_diagnostic_info const &diag, e_stack const *e) { handle_error_impl(diag, e); });
        };
        using all_error_handler_type = decltype(all_error_handler);

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
                        opC::start(io_context, async_utils::bind_ehandlers_type<all_error_handler_type>(
                                                           [&](leaf::result<void> r) -> leaf::result<void> {
                                                               leaf::remote_handle_all(
                                                                   [&]() -> leaf::result<void> {
                                                                       // NOLINTNEXTLINE
                                                                       LEAF_CHECK(r);
                                                                       std::cout << "Success" << std::endl;
                                                                       retry = false;
                                                                       return {};
                                                                   },
                                                                   [&](leaf::error_in_remote_handle_all const &error) {
                                                                       return all_error_handler(error);
                                                                   });
                                                               return {};
                                                           }));
                    },
                    [&](leaf::error_in_remote_try_ const &error) { return try_error_handler(error); });

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
