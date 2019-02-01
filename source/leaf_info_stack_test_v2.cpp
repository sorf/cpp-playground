#include "bind_ehandlers_type.hpp"
#include "leaf_call.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/leaf/all.hpp>
#include <functional>
#include <future>
#include <iostream>
#include <list>

namespace asio = boost::asio;
namespace leaf = boost::leaf;

struct e_stack {
    std::nullptr_t value;
};

std::size_t &failure_counter() {
    static std::size_t counter = 0;
    return counter;
}

void set_failure_counter(std::size_t start_count) { failure_counter() = start_count; }

void failure_point() {
    std::size_t &counter = failure_counter();

    if (counter) {
        --counter;
    }
    if (!counter) {
        throw std::runtime_error("X");
    }
}

struct operation_1 {

    template <typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        failure_point();
        asio::post(io_context, async_utils::bind_ehandlers_type_from<Handler>([h = std::forward<Handler>(h)]() mutable {
                       using error_handler_type = async_utils::associated_ehandlers_type_t<Handler>;
                       auto r = async_utils::leaf_call<void, error_handler_type>([]() -> leaf::result<void> {
                           failure_point();
                           return {};
                       });
                       h(r);
                   }));
    }
};

struct operation_2 {

    template <typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        failure_point();
        operation_1::start(
            io_context, async_utils::bind_ehandlers_type_from<Handler>(
                            [&, h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
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
        failure_point();
        operation_1::start(io_context,
                           async_utils::bind_ehandlers_type_from<Handler>(
                               [h = std::forward<Handler>(h)](leaf::result<void> r) mutable {
                                   if (r) {
                                       using error_handler_type = async_utils::associated_ehandlers_type_t<Handler>;
                                       r = async_utils::leaf_call<void, error_handler_type>([]() -> leaf::result<void> {
                                           failure_point();
                                           return {};
                                       });
                                   }
                                   h(r);
                               }));
    }
};

int main() {
    try {
        auto try_error_handler = [](leaf::error_in_remote_try_ const &error) {
            return leaf::handle_error(error, [](leaf::verbose_diagnostic_info const &diag, e_stack const *e) {
                std::cout << "Try-Error: "
                          << "any_stack: " << (e != nullptr ? "YES" : "NO") << ", diagnostic:" << diag << std::endl;
            });
        };

        auto error_handler = [&](leaf::error_in_remote_handle_all const &error) {
            return leaf::handle_error(error,
                                      [&](std::exception_ptr const &ep, e_stack const *e) {
                                          leaf::try_([&] { std::rethrow_exception(ep); },
                                                     [&](leaf::verbose_diagnostic_info const &diag) {
                                                         std::cout << "Error-Try: "
                                                                   << "any_stack: " << (e != nullptr ? "YES" : "NO")
                                                                   << ", diagnostic:" << diag << std::endl;
                                                     });
                                      },
                                      [](leaf::verbose_diagnostic_info const &diag, e_stack const *e) {
                                          std::cout << "Error: "
                                                    << "any_stack: " << (e != nullptr ? "YES" : "NO")
                                                    << ", diagnostic:" << diag << std::endl;
                                      });
        };

        bool retry = true;
        std::size_t start_count = 1;
        while (retry) {
            std::cout << "Run: " << start_count << std::endl;
            set_failure_counter(start_count++);
            try {
                asio::io_context io_context;

                leaf::remote_try_(
                    [&] {
                        operation_2::start(io_context, async_utils::bind_ehandlers_type<decltype(error_handler)>(
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
                                                                       return error_handler(error);
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
