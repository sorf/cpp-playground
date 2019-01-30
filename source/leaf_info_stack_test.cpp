#include <boost/leaf/all.hpp>
#include <future>
#include <iostream>

namespace leaf = boost::leaf;

struct e_stack {
    std::nullptr_t value;
};

template <typename ErrorHandler, typename F> decltype(auto) initiate_operation(F &&f) {
    [[maybe_unused]] auto a_initiate = leaf::accumulate([](e_stack &) { std::cout << "stack: initiate\n"; });

    return [f = std::move(f), a_initiate = std::move(a_initiate)] {
        [[maybe_unused]] auto a_c1 = leaf::accumulate([](e_stack &) { std::cout << "stack: continuation1\n"; });
        auto call = leaf::capture_in_result<ErrorHandler>([]() -> leaf::result<int> {
            return leaf::exception_to_result([]() -> leaf::result<int> {
                [[maybe_unused]] auto a_c2 = leaf::accumulate([](e_stack &) { std::cout << "stack: continuation2\n"; });
                throw std::runtime_error("X");
            });
        });
        f(call());
    };
}

int main(void) {
    auto error_handler = [](leaf::error const &error) {
        return leaf::handle_error(error, [](leaf::verbose_diagnostic_info const &diag, e_stack const *e) {
            std::cout << "Error: "
                      << "any_stack: " << (e ? "YES" : "NO") << ", diagnostic:" << diag << std::endl;
        });
    };

    auto continuation = initiate_operation<decltype(error_handler)>([&](leaf::result<int> result) {
        leaf::bound_handle_all(
            [&result]() -> leaf::result<void> {
                LEAF_CHECK(result);
                std::cout << "Success" << std::endl;
                return {};
            },
            [&](leaf::error const &error) { return error_handler(error); });
    });

    auto f = std::async(std::move(continuation));
    f.get();

    return 0;
}
