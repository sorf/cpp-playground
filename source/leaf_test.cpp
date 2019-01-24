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

struct e_failure_info {
    std::string value;
};

enum custom_error_code { first_error = 1, second_error };
namespace boost {
namespace leaf {
template <> struct is_error_type<custom_error_code> : std::true_type {};
template <> struct is_error_type<std::error_code> : std::true_type {};
} // namespace leaf
} // namespace boost

void f1(unsigned behavior) {
    [[maybe_unused]] auto propagate = leaf::preload(e_failure_info{"f1"});
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

    [[maybe_unused]] auto propagate = leaf::preload(e_failure_info{"f2"});
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
        return leaf::new_error(std::make_error_code(std::errc::address_in_use));
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
        auto result = leaf::capture_result<std::exception, e_failure_info>(
            [behavior]() -> leaf::result<int> { return f2(behavior); });
        h(result());
    }
};

int main() {
    for (unsigned i = 0; i < 10; ++i) {
        asio::io_context io_context;
        std::cout << "\nf2(" << i << ")" << std::endl;
        async_operation::start(io_context, i, [](leaf::result<int> result) {
            leaf::try_(
                [&result]() -> leaf::result<void> {
                    LEAF_CHECK(result);
                    std::cout << "Success" << std::endl;
                    return {};
                },
                [](leaf::catch_<std::runtime_error> e, e_failure_info const &info) -> leaf::result<void> {
                    std::cout << "Error: runtime_error: " << e.value.what() << ", info: " << info.value << std::endl;
                    return {};
                },
                [](leaf::catch_<std::runtime_error> e) -> leaf::result<void> {
                    std::cout << "Error: runtime_error: " << e.value.what() << std::endl;
                    return {};
                },
                [](custom_error_code ec, e_failure_info const &info) -> leaf::result<void> {
                    std::cout << "Error: code: " << ec << ", info: " << info.value << std::endl;
                    return {};
                },
                [](custom_error_code ec) -> leaf::result<void> {
                    std::cout << "Error: code: " << ec << std::endl;
                    return {};
                },
                [](leaf::error_info const &e /*, leaf::verbose_diagnostic_info const *v*/) -> leaf::result<void> {
                    std::cout << "Error: unmatched error, what: "
                              << (e.has_exception() ? e.exception()->what() : "<NA>") << std::endl;
                    /*if (v) {
                        std::cout << "Error: verbose-info: " << *v << std::endl;
                    }*/
                    return {};
                });
        });
        io_context.run();
    }
    return 0;
}
