#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/format.hpp>
#include <boost/leaf/all.hpp>
#include <boost/spirit/include/qi_numeric.hpp>
#include <boost/spirit/include/qi_parse.hpp>
#include <deque>
#include <iostream>
#include <list>
#include <optional>
#include <string>

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

// The HTTP request type.
using request_t = http::request<http::string_body>;
// The HTTP response type.
using response_t = http::response<http::string_body>;

response_t handle_request(request_t &&request);

template <class AsyncStream, typename ErrorContext, typename CompletionToken>
auto async_demo_rpc(AsyncStream &stream, ErrorContext &error_context, CompletionToken &&token) ->
    typename net::async_result<typename std::decay<CompletionToken>::type, void(leaf::result<void>)>::return_type {

    static_assert(beast::is_async_stream<AsyncStream>::value, "AsyncStream requirements not met");

    using handler_type =
        typename net::async_completion<CompletionToken, void(leaf::result<void>)>::completion_handler_type;
    using base_type = beast::stable_async_base<handler_type, beast::executor_type<AsyncStream>>;
    struct internal_op : base_type {
        // This object must have a stable address
        struct temporary_data {
            beast::flat_buffer buffer;
            std::optional<http::request_parser<request_t::body_type>> parser;
            std::optional<response_t> response;
        };

        AsyncStream &m_stream;
        ErrorContext &m_error_context;
        temporary_data &m_data;
        bool m_write_and_quit;

        internal_op(AsyncStream &stream, ErrorContext &error_context, handler_type &&handler)
            : base_type{std::move(handler), stream.get_executor()}, m_stream{stream}, m_error_context{error_context},
              m_data{beast::allocate_stable<temporary_data>(*this)}, m_write_and_quit{false} {
            start_read_request();
        }

        void operator()(error_code ec, std::size_t /*bytes_transferred*/ = 0) {
            leaf::result<bool> result_continue;
            {
                leaf::context_activator active_context{m_error_context, leaf::on_deactivation::do_not_propagate};
                auto load = leaf::preload(e_last_operation{m_data.response ? "async_demo_rpc::continuation-write"
                                                                           : "async_demo_rpc::continuation-read"});

                if (ec) {
                    result_continue = leaf::new_error(ec);
                } else {
                    result_continue = leaf::exception_to_result([&]() -> leaf::result<bool> {
                        if (!m_data.response) {
                            // Process the request we received.
                            m_data.response = handle_request(/*std::move*/ (m_data.parser->release()));
                            m_write_and_quit = m_data.response->need_eof();
                            http::async_write(m_stream, *m_data.response, std::move(*this));
                            return true;
                        }

                        // If getting here, we completed a write operation.
                        m_data.response.reset();
                        // And start reading a new message if not quitting.
                        if (!m_write_and_quit) {
                            start_read_request();
                            return true;
                        }
                        return false;
                    });
                }
                // The activation object and load_last_operation need to be reset before calling the completion handler
            }
            if (!result_continue || !*result_continue) {
                // Operation complete either successfully or due to an error.
                // We need to call the handler with the proper result type
                this->complete_now(!result_continue ? leaf::result<void>{result_continue.error()}
                                                    : leaf::result<void>{});
            }
        }

        void start_read_request() {
            m_data.parser.emplace();
            m_data.parser->body_limit(1024);
            http::async_read(m_stream, m_data.buffer, *m_data.parser, std::move(*this));
        }
    };

    auto initiation = [](auto &&completion_handler, AsyncStream *stream, ErrorContext *error_context) {
        internal_op op{*stream, *error_context, std::forward<decltype(completion_handler)>(completion_handler)};
    };

    // We are in the "initiation" part of the async operation.
    [[maybe_unused]] auto load = leaf::preload(e_last_operation{"async_demo_rpc::initiation"});
    return net::async_initiate<CompletionToken, void(leaf::result<void>)>(initiation, token, &stream, &error_context);
}

// The location of a int64 parse error.
// It refers the range of characters from which the parsing was done.
struct e_parse_int64_error {
    using location_base = std::pair<std::string_view const, std::string_view::const_iterator>;
    struct location : public location_base {
        using location_base::location_base;

        friend std::ostream &operator<<(std::ostream &os, location const &value) {
            auto const &sv = value.first;
            std::size_t pos = std::distance(sv.begin(), value.second);
            if (pos == 0) {
                os << "->\"" << sv << "\"";
            } else if (pos < sv.size()) {
                os << "\"" << sv.substr(0, pos) << "\"->\"" << sv.substr(pos) << "\"";
            } else {
                os << "\"" << sv << "\"<-";
            }
            return os;
        }
    };

    location value;
};

// Parses an integer from a string_view.
leaf::result<std::int64_t> parse_int64(std::string_view word) {
    auto const begin = word.begin();
    auto const end = word.end();
    std::int64_t value = 0;
    auto i = begin;
    bool result = boost::spirit::qi::parse(i, end, boost::spirit::long_long, value);
    if (!result || i != end) {
        return leaf::new_error(e_parse_int64_error{std::make_pair(word, i)});
    }
    return value;
}

// The command being executed while we get an error.
// It refers the range of characters from which the command was extracted.
struct e_command {
    std::string_view value;
};

// The details about an incorrect number of arguments error
// Some commands may accept a variable number of arguments (e.g. greater than 1 would mean [2, SIZE_MAX]).
struct e_unexpected_arg_count {
    struct arg_info {
        std::size_t count;
        std::size_t min;
        std::size_t max;

        friend std::ostream &operator<<(std::ostream &os, arg_info const &value) {
            os << value.count << " (required: ";
            if (value.min == value.max) {
                os << value.min;
            } else if (value.max < SIZE_MAX) {
                os << "[" << value.min << ", " << value.max << "]";
            } else {
                os << "[" << value.min << ", MAX]";
            }
            os << ")";
            return os;
        }
    };

    arg_info value;
};

// The HTTP status that should be returned in case we get into an error.
struct e_http_status {
    http::status value;
};

// The E-type that describes the `error_quit` command as an error condition.
struct e_error_quit {
    struct none_t {};
    none_t value;
};

// Processes a remote command.
leaf::result<std::string> execute_command(std::string_view line) {
    // Split the command in words.
    std::list<std::string_view> words; // or std::deque<std::string_view> words;

    char const *const ws = "\t \r\n";
    auto skip_ws = [&](std::string_view &line) {
        if (auto pos = line.find_first_not_of(ws); pos != std::string_view::npos) {
            line = line.substr(pos);
        } else {
            line = std::string_view{};
        }
    };

    skip_ws(line);
    while (!line.empty()) {
        std::string_view word;
        if (auto pos = line.find_first_of(ws); pos != std::string_view::npos) {
            word = line.substr(0, pos);
            line = line.substr(pos + 1);
        } else {
            word = line;
            line = std::string_view{};
        }

        if (!word.empty()) {
            words.push_back(word);
        }
        skip_ws(line);
    }

    static char const *const help = "Help:\n"
                                    "    error-quit                  Simulated error to end the session\n"
                                    "    sum <int64>*                Addition\n"
                                    "    sub <int64>+                Substraction\n"
                                    "    mul <int64>*                Multiplication\n"
                                    "    div <int64>+                Division\n"
                                    "    mod <int64> <int64>         Remainder\n"
                                    "    <anything else>             This message";

    if (words.empty()) {
        return std::string(help);
    }

    auto command = words.front();
    words.pop_front();

    auto load_cmd = leaf::preload(e_command{command});
    auto load_http_status = leaf::preload(e_http_status{http::status::bad_request});
    std::string response;

    if (command == "error-quit") {
        return leaf::new_error(e_error_quit{});
    } else if (command == "sum") {
        std::int64_t sum = 0;
        for (auto const &w : words) {
            LEAF_AUTO(i, parse_int64(w));
            sum += i;
        }
        response = std::to_string(sum);
    } else if (command == "sub") {
        if (words.size() < 2) {
            return leaf::new_error(e_unexpected_arg_count{words.size(), 2, SIZE_MAX});
        }
        LEAF_AUTO(sub, parse_int64(words.front()));
        words.pop_front();
        for (auto const &w : words) {
            LEAF_AUTO(i, parse_int64(w));
            sub -= i;
        }
        response = std::to_string(sub);
    } else if (command == "mul") {
        std::int64_t mul = 1;
        for (auto const &w : words) {
            LEAF_AUTO(i, parse_int64(w));
            mul *= i;
        }
        response = std::to_string(mul);
    } else if (command == "div") {
        if (words.size() < 2) {
            return leaf::new_error(e_unexpected_arg_count{words.size(), 2, SIZE_MAX});
        }
        LEAF_AUTO(div, parse_int64(words.front()));
        words.pop_front();
        for (auto const &w : words) {
            LEAF_AUTO(i, parse_int64(w));
            if (i == 0) {
                // In some cases this command execution function might throw, not just return an error.
                throw std::runtime_error{"division by zero"};
            }
            div /= i;
        }
        response = std::to_string(div);
    } else if (command == "mod") {
        if (words.size() != 2) {
            return leaf::new_error(e_unexpected_arg_count{words.size(), 2, 2});
        }
        LEAF_AUTO(i1, parse_int64(words.front()));
        words.pop_front();
        LEAF_AUTO(i2, parse_int64(words.front()));
        words.pop_front();
        if (i2 == 0) {
            // In some cases this command execution function might throw, not just return an error.
            throw leaf::exception(std::runtime_error{"division by zero"});
        }
        response = std::to_string(i1 % i2);
    } else {
        response = help;
    }

    return response;
}

std::string diagnostic_to_str(leaf::verbose_diagnostic_info const &diag) {
    auto str = boost::str(boost::format("%1%") % diag);
    boost::algorithm::replace_all(str, "\n", "\n    ");
    return "\nDetailed error diagnostic:\n----\n" + str + "\n----";
};

response_t handle_request(request_t &&request) {
    std::string response_body = leaf::try_handle_all(
        [&]() -> leaf::result<std::string> { return execute_command(request.body()); },
        [](leaf::catch_<std::exception>, leaf::verbose_diagnostic_info const &diag) { return diagnostic_to_str(diag); },
        [](leaf::verbose_diagnostic_info const &diag) { return diagnostic_to_str(diag); });
    response_body += "\n";

    response_t response{http::status::ok, request.version()};
    response.set(http::field::server, "Example-with-" BOOST_BEAST_VERSION_STRING);
    response.set(http::field::content_type, "text/plain");
    response.keep_alive(request.keep_alive());
    response.body() = response_body;
    response.prepare_payload();
    return response;
}

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
            std::cout << "Try in a different terminal:\n"
                      << "    curl " << local_endpoint.address() << ":" << local_endpoint.port() << "\nor\n"
                      << "    curl " << local_endpoint.address() << ":" << local_endpoint.port() << " -d \"sum 1 2 3\""
                      << std::endl;

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
