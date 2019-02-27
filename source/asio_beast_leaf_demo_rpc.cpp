// Example of a composed asynchronous operation which uses the LEAF library for error handling and reporting.
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/async_base.hpp>
#include <boost/beast/core/buffers_prefix.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/leaf/all.hpp>
#include <boost/predef.h>
#include <boost/spirit/include/qi_numeric.hpp>
#include <boost/spirit/include/qi_parse.hpp>
#include <deque>
#include <iostream>
#include <list>
#include <optional>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace leaf = boost::leaf;
namespace net = boost::asio;

namespace {

using error_code = boost::system::error_code;

// A bare-bones on-scope-exit utility.
template <typename F> struct [[nodiscard]] scope_exit {
    explicit scope_exit(F && f) : m_f(std::move(f)) {}
    scope_exit(scope_exit const &) = delete;
    scope_exit(scope_exit &&) = delete;
    scope_exit &operator=(scope_exit const &) = delete;
    scope_exit &operator=(scope_exit &&) = delete;
    ~scope_exit() { m_f(); }
    F const m_f;
};
template <typename F> decltype(auto) on_scope_exit(F &&f) { return scope_exit<F>(std::forward<F>(f)); }

// The location of a int64 parse error.
// It refers the range of characters from which the parsing was done.
template <typename Range> struct e_parse_int64_location {
    std::pair<Range const, typename boost::range_iterator<Range>::type> value;

    friend std::ostream &operator<<(std::ostream &os, e_parse_int64_location const &e) {
        std::string s{boost::begin(e.value.first), boost::end(e.value.first)};
        std::string_view sv(s);
        std::size_t pos = std::distance(boost::begin(e.value.first), e.value.second);
        if (pos == 0) {
            os << "err->" << sv;
        } else if (pos < sv.size()) {
            os << sv.substr(0, pos) << " err-> " << sv.substr(pos);
        } else {
            os << sv << "<-err";
        }
        return os;
    }
};

// Parses an integer from a range of characters.
template <typename Range> leaf::result<std::int64_t> parse_int64(Range const &word) {
    [[maybe_unused]] auto const begin = boost::begin(word);
    [[maybe_unused]] auto const end = boost::end(word);
    std::int64_t value = 0;
#ifndef __clang_analyzer__
    auto i = begin;
    bool result = boost::spirit::qi::parse(i, end, boost::spirit::long_long, value);
    if (!result || i != end) {
        return leaf::new_error(e_parse_int64_location<Range>{std::make_pair(word, i)});
    }
#endif
    return value;
}

// The command being executed while we get an error.
// It refers the range of characters from which the command was extracted.
template <typename Range> struct e_command { Range value; };
// The expected number or arguments for the command being executed when we get an error.
// Some commands may accept a variable number of arguments (e.g. greater than 1 would mean [2, SIZE_MAX]).
struct e_expected_arg_count {
    struct range {
        std::size_t min;
        std::size_t max;
    };

    range value;

    friend std::ostream &operator<<(std::ostream &os, e_expected_arg_count const &e) {
        if (e.value.min == e.value.max) {
            os << e.value.min;
        } else if (e.value.max < SIZE_MAX) {
            os << "[" << e.value.min << ", " << e.value.max << "]";
        } else {
            os << "[" << e.value.min << ", MAX]";
        }
        return os;
    }
};
// The actual number of arguments for the command being executed when we get an error.
struct e_arg_count {
    std::size_t value;
};

// Processes a remote command.
// Returns the response and a flag indicating this is the last command to execute.
template <typename Range> leaf::result<std::pair<std::string, bool>> execute_command(Range const &line) {
    // Split the command in words.
    // Note that split() doesn't elimintate leading and trailing empty substrings.
    std::deque<Range> words; // or std::list<Range> words;

    boost::algorithm::split(words, line, boost::is_any_of("\t \r\n"), boost::algorithm::token_compress_on);
    while (!words.empty() && boost::empty(words.front())) {
        words.pop_front();
    }
    while (!words.empty() && boost::empty(words.back())) {
        words.pop_back();
    }

    static char const *const help_with_newline = "Help:\r\n"
                                                 "    quit                        End the session\r\n"
                                                 "    sum <int64>*                Addition\r\n"
                                                 "    sub <int64>+                Substraction\r\n"
                                                 "    mul <int64>*                Multiplication\r\n"
                                                 "    div <int64>+                Division\r\n"
                                                 "    mod <int64> <int64>         Remainder\r\n"
                                                 "    <anything else>             This message\r\n";

    if (words.empty()) {
        return std::make_pair(std::string{help_with_newline}, false);
    }

    auto command = words.front();
    words.pop_front();

    auto load_cmd = leaf::preload(e_command<Range>{command});
    auto load_arg_count = leaf::preload(e_arg_count{words.size()});
    std::string response;
    bool quit = false;

    using namespace std::literals::string_view_literals;
    if (command == "quit"sv) {
        response = "quitting";
        quit = true;
    } else if (command == "sum"sv) {
        std::int64_t sum = 0;
        for (auto const &w : words) {
            LEAF_AUTO(i, parse_int64(w));
            sum += i;
        }
        response = std::to_string(sum);
    } else if (command == "sub"sv) {
        if (words.size() < 2) {
            return leaf::new_error(e_expected_arg_count{{2, SIZE_MAX}});
        }
        LEAF_AUTO(sub, parse_int64(words.front()));
        words.pop_front();
        for (auto const &w : words) {
            LEAF_AUTO(i, parse_int64(w));
            sub -= i;
        }
        response = std::to_string(sub);
    } else if (command == "mul"sv) {
        std::int64_t mul = 1;
        for (auto const &w : words) {
            LEAF_AUTO(i, parse_int64(w));
            mul *= i;
        }
        response = std::to_string(mul);
    } else if (command == "div"sv) {
        if (words.size() < 2) {
            return leaf::new_error(e_expected_arg_count{{2, SIZE_MAX}});
        }
        LEAF_AUTO(div, parse_int64(words.front()));
        words.pop_front();
        for (auto const &w : words) {
            LEAF_AUTO(i, parse_int64(w));
            if (i == 0) {
                throw std::runtime_error{"division by zero"};
            }
            div /= i;
        }
        response = std::to_string(div);
    } else if (command == "mod"sv) {
        if (words.size() != 2) {
            return leaf::new_error(e_expected_arg_count{{2, 2}});
        }
        LEAF_AUTO(i1, parse_int64(words.front()));
        words.pop_front();
        LEAF_AUTO(i2, parse_int64(words.front()));
        words.pop_front();
        if (i2 == 0) {
            throw leaf::exception(std::runtime_error{"division by zero"});
        }
        response = std::to_string(i1 % i2);
    } else {
        return std::make_pair(std::string{help_with_newline}, false);
    }

    response += "\r\n";
    return std::make_pair(response, quit);
}

// A composed asynchronous operation that implements a basic remote calculator.
// It receives from the remote side commands such as:
//      sum 1 2 3
//      div 3 2
//      mod 1 0
// and then sends back the result.
//
// It is based on:
// https://github.com/boostorg/beast/blob/b02f59ff9126c5a17f816852efbbd0ed20305930/example/echo-op/echo_op.cpp#L1
// https://github.com/chriskohlhoff/asio/blob/e7b397142ae11545ea08fcf04db3008f588b4ce7/asio/src/examples/cpp11/operations/composed_5.cpp
template <class AsyncStream, class DynamicReadBuffer, class DynamicWriteBuffer, typename CompletionToken>
auto async_demo_rpc(AsyncStream &stream, DynamicReadBuffer &read_buffer, DynamicWriteBuffer &write_buffer,
                    CompletionToken &&token) ->
    typename net::async_result<typename std::decay<CompletionToken>::type, void(error_code)>::return_type {

    static_assert(beast::is_async_stream<AsyncStream>::value, "AsyncStream requirements not met");
    static_assert(net::is_dynamic_buffer<DynamicReadBuffer>::value, "DynamicBuffer type requirements not met");
    static_assert(net::is_dynamic_buffer<DynamicWriteBuffer>::value, "DynamicBuffer type requirements not met");

    using handler_type = typename net::async_completion<CompletionToken, void(error_code)>::completion_handler_type;
    using base_type = beast::async_base<handler_type, beast::executor_type<AsyncStream>>;
    struct internal_op : base_type {
        AsyncStream &m_stream;
        DynamicReadBuffer &m_read_buffer;
        DynamicWriteBuffer &m_write_buffer;
        bool m_write_and_quit;

#if BOOST_COMP_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-local-typedefs"
#endif
        using read_buffers_type = typename DynamicReadBuffer::const_buffers_type;
        using read_buffers_iterator = net::buffers_iterator<read_buffers_type>;
        using read_buffers_range = boost::iterator_range<read_buffers_iterator>;

        using write_buffers_type = typename DynamicWriteBuffer::mutable_buffers_type;
        using write_buffers_iterator = net::buffers_iterator<write_buffers_type>;

#if BOOST_COMP_CLANG
#pragma clang diagnostic pop
#endif

        internal_op(AsyncStream &stream, DynamicReadBuffer &read_buffer, DynamicWriteBuffer &write_buffer,
                    handler_type &&handler)
            : base_type{std::move(handler), stream.get_executor()}, m_stream(stream), m_read_buffer(read_buffer),
              m_write_buffer(write_buffer), m_write_and_quit{false} {
            start_read_some();
        }

        void start_read_some() {
            std::size_t bytes_to_read = 3; // A small value for testing purposes
                                           // (multiple `operator()` calls for most messages)
            m_stream.async_read_some(m_read_buffer.prepare(bytes_to_read), std::move(*this));
        }

        void operator()(error_code ec, std::size_t bytes_transferred = 0) {
            if (!ec) {
                if (m_write_buffer.size() == 0) {
                    // We read something.
                    m_read_buffer.commit(bytes_transferred);
                    std::size_t pos_nl = find_newline(m_read_buffer.data());
                    if (pos_nl == 0) {
                        // Read some more until we get a newline
                        return start_read_some();
                    }

                    // Process the line we received.
                    auto line = beast::buffers_prefix(pos_nl, m_read_buffer.data());
                    read_buffers_range range_line{read_buffers_iterator::begin(line), read_buffers_iterator::end(line)};
                    auto r = execute_command(range_line);
                    std::string response;
                    std::tie(response, m_write_and_quit) = r.value();
                    m_read_buffer.consume(pos_nl);

                    // Prepare the response buffer
                    std::size_t write_size = response.size();
                    auto response_buffers = m_write_buffer.prepare(write_size);
                    std::copy(response.begin(), response.end(), write_buffers_iterator::begin(response_buffers));
                    m_write_buffer.commit(write_size);
                    return net::async_write(m_stream, response_buffers, std::move(*this));
                }

                // If getting here, we completed a write operation.
                assert(m_write_buffer.size() == bytes_transferred);
                m_write_buffer.consume(bytes_transferred);
                // And start reading a new message if not quitting.
                if (!m_write_and_quit) {
                    return start_read_some();
                }
            }
            // Operation complete if we get here.
            this->invoke_now(ec);
        }

        // Same as:
        // https://github.com/boostorg/beast/blob/c82237512a95487fd67a4287f79f4458ba978f43/example/echo-op/echo_op.cpp#L199
        static std::size_t find_newline(read_buffers_type const &buffers) {
            auto begin = read_buffers_iterator::begin(buffers);
            auto end = read_buffers_iterator::end(buffers);
            auto result = std::find(begin, end, '\n');
            if (result == end) {
                return 0;
            }
            return result + 1 - begin;
        }
    };

    typename net::async_completion<CompletionToken, void(error_code)> init(token);
    internal_op op{stream, read_buffer, write_buffer, std::move(init.completion_handler)};
    return init.result.get();
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: " << argv[0] << " <address> <port>" << std::endl;    // NOLINT
            std::cerr << "Example:\n    " << argv[0] << " 0.0.0.0 8080" << std::endl; // NOLINT
            return -1;
        }

        auto const address{net::ip::make_address(argv[1])};              // NOLINT
        auto const port{static_cast<std::uint16_t>(std::atoi(argv[2]))}; // NOLINT
        net::ip::tcp::endpoint const endpoint{address, port};

        net::io_context io_context;

        // Set up a worker thread that runs the io_context in background.
        auto threads_io_work = net::make_work_guard(io_context);
        std::thread thread_io_context{[&io_context] {
            try {
                io_context.run();
            } catch (std::exception const &e) {
                std::cerr << "Server-thread error: " << e.what() << std::endl;
            }
        }};
        // And our cleanup work at the end of the scope to stop this thread.
        auto cleanup = on_scope_exit([&] {
            threads_io_work.reset();
            thread_io_context.join();
        });

        // Start the server acceptor and wait for a client.
        net::ip::tcp::acceptor acceptor{io_context, endpoint};
        auto local_endpoint = acceptor.local_endpoint();
        std::cout << "Server: Started on: " << local_endpoint << std::endl;
        std::cout << "Try in a different terminal:\n    telnet " << local_endpoint.address() << " "
                  << local_endpoint.port() << "\n    help<ENTER>" << std::endl;

#ifndef __clang_analyzer__
        auto socket = acceptor.accept();
#else
        net::ip::tcp::socket socket{io_context};
#endif
        std::cout << "Server: Client connected: " << socket.remote_endpoint() << std::endl;

        // Start the `async_demo_rpc` operation and wait for its completion.
        beast::flat_buffer read_buffer;
        beast::flat_buffer write_buffer;
        std::future<void> f = async_demo_rpc(socket, read_buffer, write_buffer, net::use_future);
        try {
            f.get();
            std::cout << "Server: Client work completed successfully" << std::endl;
        } catch (std::exception const &e) {
            std::cout << "Server: Client work completed with error: " << e.what() << std::endl;
        }

        // Let the remote side know we are shutting down.
        error_code ignored;
        socket.shutdown(net::ip::tcp::socket::shutdown_both, ignored);

    } catch (std::exception const &e) {
        std::cerr << "Server error: " << e.what() << std::endl;
    }
    return 0;
}
