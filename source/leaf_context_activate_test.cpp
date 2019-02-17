#include "on_scope_exit.hpp"
#include <boost/leaf/all.hpp>
#include <iostream>

namespace leaf = boost::leaf;

template <class TryBlock, class RemoteH>
typename std::decay<decltype(std::declval<TryBlock>()())>::type
leaf_remote_try_handle_some_no_lint(TryBlock &&try_block, RemoteH &&h) {
#ifndef __clang_analyzer__
    return leaf::remote_try_handle_some(std::forward<TryBlock>(try_block), std::forward<RemoteH>(h));
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

leaf::result<void> test_f() {
    static unsigned c = 0;
    switch (c++ % 3) {
    case 0:
        throw std::runtime_error("exception");
    case 1:
        return std::make_error_code(std::errc::address_in_use);
    default:
        break;
    }
    return {};
}

int main() {
    try {
        auto error_handler = [&](leaf::error_info const &error, unsigned line) {
            return leaf::remote_handle_some(
                error, [&](/*leaf::catch_<std::exception>, */leaf::verbose_diagnostic_info const &diag) {
                    std::cout << "\n-----\nerror handled at line:" << line << "\ndiagnostic" << diag << std::endl;
                    return leaf::result<void>{};
                });
        };

        bool retry = true;
        while (retry) {
            leaf_remote_try_handle_some_no_lint(
                [&]() -> leaf::result<void> {
                    auto error_context = leaf::make_context(&error_handler);
                    error_context.activate();
                    auto error_context_deactivate = async_utils::on_scope_exit([&] { error_context.deactivate(true); });

                    auto r = test_f();
                    if (r) {
                        std::cout << "\n----\nSuccess" << std::endl;
                        retry = false;
                    } else {
                        error_context_deactivate.clear();
                        error_context.deactivate(false);
                        error_context.remote_handle_some(
                            r, [&](leaf::error_info const &error) { return error_handler(error, __LINE__); });
                    }
                    return {};
                },
                [&](leaf::error_info const &error) { return error_handler(error, __LINE__); });
        }
    } catch (std::exception const &e) {
        std::cout << "Fatal Error: " << e.what() << "\n";
    }
    return 0;
}