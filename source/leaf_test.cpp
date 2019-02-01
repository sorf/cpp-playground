#include "bind_ehandlers_type.hpp"
#include "leaf_call.hpp"

#include <algorithm>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/assert.hpp>
#include <boost/leaf/all.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace asio = boost::asio;
namespace leaf = boost::leaf;

using info_stack_type = std::vector<std::string_view>;

struct e_failure_info_stack {
    // The operations executed in leaf::accumulate must not throw.
    void push_back(std::string_view info) noexcept {
        try {
            value.push_back(info);
        } catch (...) {
        }
    }

    info_stack_type value;
};

std::string info_stack_to_string(e_failure_info_stack const *info_stack) {
    if (info_stack == nullptr) {
        return "";
    }
    std::string str;
    for (auto const &s : boost::adaptors::reverse(info_stack->value)) {
        if (!str.empty()) {
            str += ">";
        }
        str += s;
    }
    return str;
}

enum custom_error_code { first_error = 1, second_error };
namespace boost {
namespace leaf {
template <> struct is_e_type<custom_error_code> : std::true_type {};
} // namespace leaf
} // namespace boost

void f1(unsigned behavior) {
    [[maybe_unused]] auto p2 = leaf::accumulate([](e_failure_info_stack &info) { info.push_back("f1"); });

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
        throw std::runtime_error("what-f2-1");
    }

    [[maybe_unused]] auto p2 = leaf::accumulate([](e_failure_info_stack &info) { info.push_back("f2"); });

    switch (behavior) {
    case 0:
        break;
    case 1:
        break;
    case 2:
        return leaf::new_error(second_error);
    case 3:
        throw std::runtime_error("what-f2-2");
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
    template <typename LeafAccumulator, typename Handler>
    static void start(asio::io_context &io_context, unsigned behavior, LeafAccumulator &&acc, Handler &&h) {
        auto next_acc = async_utils::defer_accumulate(
            std::forward<LeafAccumulator>(acc), [](e_failure_info_stack &info) { info.push_back("async_operation"); });

        auto executor = get_executor(io_context, h);
        asio::post(io_context, async_utils::bind_ehandlers_type_from<Handler>(asio::bind_executor(
                                   executor, [=, acc = std::move(next_acc), h = std::forward<Handler>(h)]() mutable {
                                       execute(behavior, std::move(acc), std::move(h));
                                   })));
    }

  private:
    template <typename Handler, typename LeafAccumulator>
    static void execute(unsigned behavior, LeafAccumulator &&acc, Handler &&h) {
        auto next_acc =
            async_utils::defer_accumulate(std::forward<LeafAccumulator>(acc), [](e_failure_info_stack &info) {
                info.push_back("async_operation::execute");
            });

        h(async_utils::leaf_call<unsigned>(std::move(next_acc), async_utils::bind_ehandlers_type_from<Handler>(
                                                                    [&]() mutable { return f2(behavior); })));
    }
};

auto make_error_handler() {
    return [](leaf::error_in_remote_handle_all const &error) {
        return leaf::handle_error(
            error,
            [](custom_error_code ec, e_failure_info_stack const *info_stack) {
                std::cout << "Error: custom_error code: " << ec << ", info_stack: " << info_stack_to_string(info_stack)
                          << std::endl;
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
                                         << ", info_stack: " << info_stack_to_string(info_stack) << std::endl;
                           },
                           [&](leaf::error_info const &e, leaf::verbose_diagnostic_info const &diag) {
                               std::cout << "Error: unmatched exception, what: "
                                         << (e.has_exception() ? e.exception()->what() : "<NA>")
                                         << ", info_stack: " << info_stack_to_string(info_stack)
                                         << ", diagnostic:" << diag << std::endl;
                           });
            },
            // Note: This needs to be the last (or second to last) as it would match any error -
            //       leaf::error_id is a std::error_code.
            //       On the other side, leaf::verbose_diagnostic_info below prints the original
            //       error_code due to it being a first-class error type in LEAF.
            [](std::error_code const &ec) {
                std::cout << "Error: error_code: " << ec << ":" << ec.message() << std::endl;
            },
            [](leaf::verbose_diagnostic_info const &diag, e_failure_info_stack const *info_stack) {
                std::cout << "Error: unmatched error"
                          << ", info_stack: " << info_stack_to_string(info_stack) << ", diagnostic:" << diag
                          << std::endl;
            });
    };
}

int main() {
    try {
        for (unsigned i = 0; i < 10; ++i) {
            asio::io_context io_context;
            std::cout << "\nf2(" << i << ")" << std::endl;

            auto ehandlers = make_error_handler();
            auto acc = async_utils::defer_accumulate([](e_failure_info_stack &info) { info.push_back("main"); });
            async_operation::start(
                io_context, i, std::move(acc),
                async_utils::bind_ehandlers_type<decltype(ehandlers)>([&](leaf::result<int> result) {
                    leaf::remote_handle_all(
                        [&result]() -> leaf::result<void> {
                            // NOLINTNEXTLINE
                            LEAF_CHECK(result);
                            std::cout << "Success" << std::endl;
                            return {};
                        },
                        [&](leaf::error_in_remote_handle_all const &error) { return ehandlers(error); });
                }));
            io_context.run();
        }
    } catch (std::exception const &e) {
        std::cout << "Error: " << e.what() << "\n";
    }
    return 0;
}
