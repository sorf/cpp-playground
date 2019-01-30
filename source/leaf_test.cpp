#include "bind_ehandlers_type.hpp"
#include "leaf_call.hpp"

#include <algorithm>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/assert.hpp>
#include <boost/leaf/all.hpp>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace asio = boost::asio;
namespace leaf = boost::leaf;

using info_stack_type = std::vector<std::string_view>;

struct [[nodiscard]] info_stack_appender {
    explicit info_stack_appender(info_stack_type * info_stack, std::string_view info)
        : m_info_stack(info_stack), m_info(info) {
        if (m_info_stack) {
            m_info_stack->push_back(info);
        }
    }

    info_stack_appender(info_stack_appender const &) = delete;
    info_stack_appender(info_stack_appender && other) : m_info_stack(other.m_info_stack), m_info(other.m_info) {
        other.m_info_stack = nullptr;
    }

    info_stack_appender &operator=(info_stack_appender const &) = delete;
    info_stack_appender &operator=(info_stack_appender &&other) = delete;
    ~info_stack_appender() { reset(); }

    void reset() noexcept {
        if (m_info_stack) {
            BOOST_ASSERT(m_info_stack->back() == m_info);
            m_info_stack->pop_back();
        }
        m_info_stack = nullptr;
    }

  private:
    info_stack_type *m_info_stack;
    std::string_view m_info;
};

std::string info_stack_to_string(info_stack_type const &info_stack) {
    std::string str;
    for (auto s : info_stack) {
        if (!str.empty()) {
            str += ">";
        }
        str += s;
    }
    return str;
}

info_stack_type *&tl_get_info_stack() noexcept {
    static thread_local info_stack_type *info_stack = nullptr;
    return info_stack;
}

void tl_set_info_stack(info_stack_type &info_stack) noexcept { tl_get_info_stack() = &info_stack; }
void tl_set_info_stack(info_stack_type *info_stack) noexcept { tl_get_info_stack() = info_stack; }

decltype(auto) tl_append_info_stack(std::string_view info) {
    return info_stack_appender(tl_get_info_stack(), info);
}

struct e_failure_info_stack {
    e_failure_info_stack() {
        if (auto info_stack = tl_get_info_stack()) {
            value = *info_stack;
        }
    }
    e_failure_info_stack(e_failure_info_stack const &) = delete;
    e_failure_info_stack(e_failure_info_stack &&) = default;
    e_failure_info_stack &operator=(e_failure_info_stack const &) = delete;
    e_failure_info_stack &operator=(e_failure_info_stack &&other) = delete;

    info_stack_type value;
};

std::string info_stack_to_string(e_failure_info_stack const *info_stack) {
    return info_stack != nullptr ? info_stack_to_string(info_stack->value) : "";
}

enum custom_error_code { first_error = 1, second_error };
namespace boost {
namespace leaf {
template <> struct is_e_type<custom_error_code> : std::true_type {};
} // namespace leaf
} // namespace boost

void f1(unsigned behavior) {
    [[maybe_unused]] auto p1 = leaf::defer([] { return e_failure_info_stack(); });
    [[maybe_unused]] auto p2 = leaf::accumulate([](e_failure_info_stack &info) { info.value.push_back("f1"); });

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

    [[maybe_unused]] auto p1 = leaf::defer([] { return e_failure_info_stack(); });
    [[maybe_unused]] auto p2 = leaf::accumulate([](e_failure_info_stack &info) { info.value.push_back("f2"); });

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
    template <typename Handler> static void start(asio::io_context &io_context, unsigned behavior, Handler &&h) {
        auto info_stack = tl_get_info_stack();
        auto op_info = tl_append_info_stack("async_operation");

        asio::post(io_context, async_utils::bind_ehandlers_type_from<Handler>(asio::bind_executor(
                                   get_executor(io_context, h),
                                   [=, h = std::forward<Handler>(h), op_info = std::move(op_info)]() mutable {
                                       tl_set_info_stack(info_stack);
                                       execute(behavior, std::move(h));
                                   })));
    }

  private:
    template <typename Handler> static void execute(unsigned behavior, Handler &&h) {
        [[maybe_unused]] auto info = tl_append_info_stack("execute1");

        h(async_utils::leaf_call<unsigned>(async_utils::bind_ehandlers_type_from<Handler>([&]() mutable {
            [[maybe_unused]] auto p1 = leaf::defer([] { return e_failure_info_stack(); });
            [[maybe_unused]] auto p2 = leaf::accumulate([](e_failure_info_stack &info) { info.value.push_back("execute2"); });
            return f2(behavior);
        })));
    }
};

auto make_ehandlers() {
    return [](leaf::error const &error) {
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

            info_stack_type info_stack;
            tl_set_info_stack(info_stack);
            [[maybe_unused]] auto info = tl_append_info_stack("main");
            auto ehandlers = make_ehandlers();
            async_operation::start(io_context, i,
                                   async_utils::bind_ehandlers_type<decltype(ehandlers)>([&](leaf::result<int> result) {
                                       leaf::bound_handle_all(
                                           [&result]() -> leaf::result<void> {
                                               // NOLINTNEXTLINE
                                               LEAF_CHECK(result);
                                               std::cout << "Success" << std::endl;
                                               return {};
                                           },
                                           [&](leaf::error const &error) { return ehandlers(error); });
                                   }));
            io_context.run();
        }
    } catch (std::exception const &e) {
        std::cout << "Error: " << e.what() << "\n";
    }
    return 0;
}
