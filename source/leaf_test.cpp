#include <algorithm>
#include <boost/leaf/all.hpp>
#include <iostream>
#include <string>
#include <system_error>

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

int main() {

    for (unsigned i = 0; i < 10; ++i) {
        std::cout << "\nf2(" << i << ")" << std::endl;
        leaf::result<int> result = leaf::try_(
            [i]() -> leaf::result<int> {
                LEAF_CHECK(f2(i));
                std::cout << "Success" << std::endl;
                return 0;
            },
            [](leaf::catch_<std::runtime_error> e, e_failure_info const &info) -> leaf::result<int> {
                std::cout << "Error: runtime_error: " << e.value.what() << ", info: " << info.value << std::endl;
                return -1;
            },
            [](leaf::catch_<std::runtime_error> e) -> leaf::result<int> {
                std::cout << "Error: runtime_error: " << e.value.what() << std::endl;
                return -2;
            },
            [](custom_error_code ec, e_failure_info const &info) -> leaf::result<int> {
                std::cout << "Error: code: " << ec << ", info: " << info.value << std::endl;
                return -3;
            },
            [](custom_error_code ec) -> leaf::result<int> {
                std::cout << "Error: code: " << ec << std::endl;
                return -4;
            },
            [](leaf::error_info const &e /*, leaf::verbose_diagnostic_info const *v*/) -> leaf::result<int> {
                std::cout << "Error: unmatched error, what: " << (e.has_exception() ? e.exception()->what() : "<NA>")
                          << std::endl;
                /*if (v) {
                    std::cout << "Error: verbose-info: " << *v << std::endl;
                }*/
                return -5;
            });
    }
    return 0;
}
