#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/format.hpp>
#include <boost/leaf/all.hpp>
#include <iostream>
#include <string>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace leaf = boost::leaf;
namespace net = boost::asio;

namespace {
using error_code = boost::system::error_code;
} // namespace

namespace boost {
namespace leaf {
// Ensure the error_code is an E-type.
template <> struct is_e_type<error_code> : std::true_type {};
} // namespace leaf
} // namespace boost

// The operation being performed when an error occurs.
struct e_last_operation {
    std::string_view value;
};
// And the type leaf::preload() returns for it.
using preload_last_operation_t = std::decay_t<decltype(leaf::preload(e_last_operation{""}))>;

template <class AsyncStream, typename ErrorContext, typename CompletionToken>
auto async_demo_rpc(AsyncStream &stream, ErrorContext &error_context, CompletionToken &&token) ->
    typename net::async_result<typename std::decay<CompletionToken>::type, void(leaf::result<void>)>::return_type {

    static_assert(beast::is_async_stream<AsyncStream>::value, "AsyncStream requirements not met");

    using handler_type =
        typename net::async_completion<CompletionToken, void(leaf::result<void>)>::completion_handler_type;
    using base_type = beast::async_base<handler_type, beast::executor_type<AsyncStream>>;
    struct internal_op : base_type {
        AsyncStream &m_stream;
        ErrorContext &m_error_context;

        internal_op(AsyncStream &stream, ErrorContext &error_context, handler_type &&handler)
            : base_type{std::move(handler), stream.get_executor()}, m_stream{stream}, m_error_context{error_context} {
            {}
        }
    };

    auto initiation = [](auto &&completion_handler, AsyncStream *stream, ErrorContext *error_context) {
        internal_op op{*stream, *error_context, std::forward<decltype(completion_handler)>(completion_handler)};
    };

    // We are in the "initiation" part of the async operation.
    [[maybe_unused]] auto load = leaf::preload(e_last_operation{"async_demo_rpc::initiation"});
    return net::async_initiate<CompletionToken, void(leaf::result<void>)>(initiation, token, &stream, &error_context);
}

std::string diagnostic_to_str(leaf::verbose_diagnostic_info const &diag) {
    auto str = boost::str(boost::format("%1%") % diag);
    boost::algorithm::replace_all(str, "\n", "\n    ");
    return "\nDetailed error diagnostic:\n----\n" + str + "\n----";
};

int main(int argc, char **argv) {
    auto e_prefix = [](e_last_operation const *op) {
        if (op != nullptr) {
            return boost::str(boost::format("Error (%1%): ") % op->value);
        }
        return std::string("Error: ");
    };

    // Error handler for internal server internal errors (not communicated to the remote client).
    auto error_handler = [&](leaf::error_info const &error) {
        return leaf::remote_handle_all(
            error,
            [&](std::exception_ptr const &ep, e_last_operation const *op) {
                return leaf::try_handle_all(
                    [&]() -> leaf::result<int> { std::rethrow_exception(ep); },
                    [&](leaf::catch_<std::exception> e, leaf::verbose_diagnostic_info const &diag) {
                        std::cerr << e_prefix(op) << e.value().what() << " (captured)" << diagnostic_to_str(diag)
                                  << std::endl;
                        return -11;
                    },
                    [&](leaf::verbose_diagnostic_info const &diag) {
                        std::cerr << e_prefix(op) << "unknown (captured)" << diagnostic_to_str(diag) << std::endl;
                        return -12;
                    });
            },
            [&](leaf::catch_<std::exception> e, e_last_operation const *op, leaf::verbose_diagnostic_info const &diag) {
                std::cerr << e_prefix(op) << e.value().what() << diagnostic_to_str(diag) << std::endl;
                return -21;
            },
            [&](error_code ec, leaf::verbose_diagnostic_info const &diag, e_last_operation const *op) {
                std::cerr << e_prefix(op) << ec << ":" << ec.message() << diagnostic_to_str(diag) << std::endl;
                return -22;
            },
            [&](leaf::verbose_diagnostic_info const &diag, e_last_operation const *op) {
                std::cerr << e_prefix(op) << "unknown" << diagnostic_to_str(diag) << std::endl;
                return -23;
            });
    };

    auto error_context = leaf::make_context(&error_handler);

    // Top level try block and error handler.
    // It will handle errors from starting the server for example failure to bind to a given port
    // (e.g. ports less than 1024 if not running as root)
    return leaf::remote_try_handle_all(
        [&]() -> leaf::result<int> {
            auto load = leaf::preload(e_last_operation{"main"});
            if (argc != 3) {
                std::cerr << "Usage: " << argv[0] << " <address> <port>" << std::endl;
                std::cerr << "Example:\n    " << argv[0] << " 0.0.0.0 8080" << std::endl;
                return -1;
            }

            auto const address{net::ip::make_address(argv[1])};
            auto const port{static_cast<std::uint16_t>(std::atoi(argv[2]))};
            net::ip::tcp::endpoint const endpoint{address, port};

            net::io_context io_context;

            // Start the server acceptor and wait for a client.
            net::ip::tcp::acceptor acceptor{io_context, endpoint};
            auto local_endpoint = acceptor.local_endpoint();
            std::cout << "Server: Started on: " << local_endpoint << std::endl;
            std::cout << "Try in a different terminal:\n    curl " << local_endpoint.address() << ":"
                      << local_endpoint.port() << "\n    help<ENTER>" << std::endl;

            auto socket = acceptor.accept();
            std::cout << "Server: Client connected: " << socket.remote_endpoint() << std::endl;

            int rv = 0;
            async_demo_rpc(socket, error_context, [&](leaf::result<void> result) {
                // Handle errors from running the server logic (e.g. client disconnecting abruptly)
                leaf::context_activator active_context(error_context, leaf::on_deactivation::do_not_propagate);
                if (result) {
                    std::cout << "Server: Client work completed successfully" << std::endl;
                    rv = 0;
                } else {
                    leaf::result<int> result_int{result.error()};
                    rv = error_context.remote_handle_all(
                        result_int, [&](leaf::error_info const &error) { return error_handler(error); });
                }
            });
            io_context.run();

            // Let the remote side know we are shutting down.
            error_code ignored;
            socket.shutdown(net::ip::tcp::socket::shutdown_both, ignored);
            return rv;
        },
        [&](leaf::error_info const &error) { return error_handler(error); });
}
