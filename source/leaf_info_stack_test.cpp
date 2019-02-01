#include <boost/leaf/all.hpp>
#include <future>
#include <iostream>

namespace leaf = boost::leaf;

struct e_stack {
    std::nullptr_t value;
};

template <typename F> decltype(auto) defer_accumulate(F &&f) {
    return [f = std::forward<F>(f)] { return leaf::accumulate(std::move(f)); };
}

template <typename LeafAccumulator, typename F> decltype(auto) defer_accumulate(LeafAccumulator &&acc, F &&f) {
    return [prev_acc = std::forward<LeafAccumulator>(acc), f = std::forward<F>(f)] {
        return std::make_pair(leaf::accumulate(std::move(f)), prev_acc());
    };
}

template <typename ErrorHandler, typename F> decltype(auto) initiate_operation(F &&f) {
    auto acc = defer_accumulate([](e_stack &) { std::cout << "stack: initiate\n"; });

    return [f = std::forward<F>(f), acc_prev = std::move(acc)] {
        auto acc = defer_accumulate(std::move(acc_prev), [](e_stack &) { std::cout << "stack: continuation1\n"; });

        auto r = leaf::capture_in_result<ErrorHandler>([acc_prev = std::move(acc)]() -> leaf::result<int> {
            auto acc = defer_accumulate(std::move(acc_prev), [](e_stack &) { std::cout << "stack: continuation2\n"; });

            return leaf::exception_to_result([acc_prev = std::move(acc)]() -> leaf::result<int> {
                auto acc =
                    defer_accumulate(std::move(acc_prev), [](e_stack &) { std::cout << "stack: continuation3\n"; });

                [[maybe_unused]] auto acc_now = acc();
                throw std::runtime_error("X");
            });
        });
        f(r);
    };
}

int main() {
    try {
        auto error_handler = [](leaf::error_in_remote_handle_all const &error) {
            return leaf::handle_error(error, [](leaf::verbose_diagnostic_info const &diag, e_stack const *e) {
                std::cout << "Error: "
                          << "any_stack: " << (e != nullptr ? "YES" : "NO") << ", diagnostic:" << diag << std::endl;
            });
        };

        auto continuation = initiate_operation<decltype(error_handler)>([&](leaf::result<int> result) {
            leaf::remote_handle_all(
                [&result]() -> leaf::result<void> {
                    // NOLINTNEXTLINE
                    LEAF_CHECK(result);
                    std::cout << "Success" << std::endl;
                    return {};
                },
                [&](leaf::error_in_remote_handle_all const &error) { return error_handler(error); });
        });

        auto f = std::async(std::move(continuation));
        f.get();
    } catch (std::exception const &e) {
        std::cout << "Error: " << e.what() << "\n";
    }
    return 0;
}
