#include <algorithm>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/leaf/all.hpp>
#include <iostream>
#include <string>
#include <system_error>

namespace asio = boost::asio;
namespace leaf = boost::leaf;

struct e_failure_info_stack {
    std::string value;
};

enum custom_error_code { first_error = 1, second_error };
namespace boost {
namespace leaf {
template <> struct is_error_type<custom_error_code> : std::true_type {};
} // namespace leaf
} // namespace boost

void f1(unsigned behavior) {
    [[maybe_unused]] auto p = leaf::accumulate([](e_failure_info_stack &info) { info.value += "|f1"; });

    switch (behavior) {
    case 0:
        return;
    case 1:
        throw std::runtime_error("f1-1");
    case 2:
        throw std::bad_alloc();
    default:
        break;
    }
}

leaf::result<unsigned> f2(unsigned behavior) {
    if (behavior == 0) {
        return leaf::new_error(first_error);
    }
    if (behavior == 1) {
        throw std::runtime_error("f2-1");
    }

    [[maybe_unused]] auto p = leaf::accumulate([](e_failure_info_stack &info) { info.value += "|f2"; });

    switch (behavior) {
    case 0:
        break;
    case 1:
        break;
    case 2:
        return leaf::new_error(second_error);
    case 3:
        throw std::runtime_error("f2-2");
    case 4:
        return leaf::new_error(std::errc::address_in_use);
    default:
        f1(behavior - 5);
        break;
    }
    return behavior;
}

struct async_operation {
  private:
    template <typename Handler> static decltype(auto) get_executor(asio::io_context &io_context, Handler &h) {
        return asio::get_associated_executor(h, io_context.get_executor());
    }

  public:
    template <typename Handler> static void start(asio::io_context &io_context, unsigned behavior, Handler &&h) {
        asio::post(io_context,
                   asio::bind_executor(get_executor(io_context, h), [behavior, h = std::forward<Handler>(h)]() mutable {
                       execute(behavior, std::move(h));
                   }));
    }

  private:
    template <typename Handler> static void execute(unsigned behavior, Handler &&h) {
        // exception_to_result now always sends the original exception in a std::exception_ptr, so we need to add that
        // to the type list passed to capture_in_result.
        auto f = leaf::capture_in_result<std::exception, e_failure_info_stack, custom_error_code, std::exception_ptr>(
            [behavior]() -> leaf::result<unsigned> {
                return leaf::exception_to_result([behavior]() -> leaf::result<unsigned> {
                    [[maybe_unused]] auto p =
                        leaf::accumulate([](e_failure_info_stack &info) { info.value += "|execute"; });
                    return f2(behavior);
                });
            });

        leaf::result<unsigned> r = f();
        h(r);
    }
};

int main() {
    std::cout << "std::errc::address_in_use is: " << (int)std::errc::address_in_use << std::endl;

    for (unsigned i = 0; i < 10; ++i) {
        asio::io_context io_context;
        std::cout << "\nf2(" << i << ")" << std::endl;
        async_operation::start(io_context, i, [](leaf::result<int> result) {
            leaf::handle_all(
                [&result]() -> leaf::result<void> {
                    LEAF_CHECK(result);
                    std::cout << "Success" << std::endl;
                    return {};
                },
                [](custom_error_code ec, e_failure_info_stack const *info_stack) {
                    std::cout << "Error: custom_error code: " << ec
                              << ", info_stack: " << (info_stack ? info_stack->value : "NA") << std::endl;
                },
                // Note:
                // This handler will be called if there is an std::exception_ptr available, which there will be in
                // case exception_to_result caught an exception. Note that any exception types from the list to
                // instantiate the exception_to_result template, if caught, will be sliced and sent as error
                // objects, so they could be intercepted in the above handlers as well (provided that they were
                // included in the type list passed to capture_in_result).
                [](std::exception_ptr const &ep, e_failure_info_stack const *info_stack) {
                    leaf::try_([&] { std::rethrow_exception(ep); },
                               [&](leaf::catch_<std::runtime_error> e) {
                                   std::cout << "Error: runtime_error: " << e.value().what()
                                             << ", info_stack: " << (info_stack ? info_stack->value : "NA")
                                             << std::endl;
                               },
                               [&](leaf::error_info const &e, leaf::verbose_diagnostic_info const &diag) {
                                   std::cout << "Error: unmatched exception, what: "
                                             << (e.has_exception() ? e.exception()->what() : "<NA>")
                                             << ", info_stack: " << (info_stack ? info_stack->value : "NA")
                                             << ", diagnostic:" << diag << std::endl;
                               });
                },
#if 0                
                // Note: This needs to be the last (or second to last) as it would match any error -
                //       leaf::error_id is a std::error_code.
                //       On the other side, leaf::verbose_diagnostic_info below prints the original
                //       error_code due to it being a first-class error type in LEAF.
                [](std::error_code const &ec) { std::cout << "Error: error_code: " << ec << std::endl; },
#endif
                [](leaf::verbose_diagnostic_info const &diag, e_failure_info_stack const *info_stack) {
                    std::cout << "Error: unmatched error"
                              << ", info_stack: " << (info_stack ? info_stack->value : "NA") << ", diagnostic:" << diag
                              << std::endl;
                });
        });
        io_context.run();
    }
    return 0;
}
