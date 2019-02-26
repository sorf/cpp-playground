#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/trim_all.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/async_op_base.hpp>
#include <boost/beast/core/buffers_prefix.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/predef.h>
#include <charconv>
#include <iostream>
#include <optional>
#include <thread>

namespace net = boost::asio;
namespace beast = boost::beast;

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

// A composed that implements a demo server side remote-procedure call.
// It is based on:
// https://github.com/boostorg/beast/blob/c82237512a95487fd67a4287f79f4458ba978f43/example/echo-op/echo_op.cpp
// https://github.com/chriskohlhoff/asio/blob/e7b397142ae11545ea08fcf04db3008f588b4ce7/asio/src/examples/cpp11/operations/composed_5.cpp
template <class AsyncStream, class DynamicReadBuffer, class DynamicWriteBuffer, typename CompletionToken>
auto async_demo_rpc(AsyncStream &stream, DynamicReadBuffer &read_buffer, DynamicWriteBuffer &write_buffer,
                    CompletionToken &&token) ->
    typename net::async_result<typename std::decay<CompletionToken>::type, void(error_code)>::return_type {

    static_assert(beast::is_async_stream<AsyncStream>::value, "AsyncStream requirements not met");
    static_assert(net::is_dynamic_buffer<DynamicReadBuffer>::value, "DynamicBuffer type requirements not met");
    static_assert(net::is_dynamic_buffer<DynamicWriteBuffer>::value, "DynamicBuffer type requirements not met");

    using handler_type = typename net::async_completion<CompletionToken, void(error_code)>::completion_handler_type;
    using base_type = beast::async_op_base<handler_type, beast::executor_type<AsyncStream>>;
    struct internal_op : base_type {
        AsyncStream &m_stream;
        DynamicReadBuffer &m_read_buffer;
        DynamicWriteBuffer &m_write_buffer;

#if BOOST_COMP_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-local-typedefs"
#endif
        using read_buffers_type = typename DynamicReadBuffer::const_buffers_type;
        using read_buffers_iterator = net::buffers_iterator<read_buffers_type>;

        using write_buffers_type = typename DynamicWriteBuffer::mutable_buffers_type;
        using write_buffers_iterator = net::buffers_iterator<write_buffers_type>;

#if BOOST_COMP_CLANG
#pragma clang diagnostic pop
#endif

        internal_op(AsyncStream &stream, DynamicReadBuffer &read_buffer, DynamicWriteBuffer &write_buffer,
                    handler_type &&handler)
            : base_type{std::move(handler), stream.get_executor()}, m_stream(stream), m_read_buffer(read_buffer),
              m_write_buffer(write_buffer) {
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
                    std::string response = process_line(line);
                    response += "\n";
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
                // And start reading a new message.
                return start_read_some();
            }
            // Operation complete here.
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

        static std::string process_line(beast::buffers_prefix_view<read_buffers_type> const &line) {
            // Converting the input line to a string for simplified processing.
            std::string line_str = beast::buffers_to_string(line);
            boost::algorithm::trim_all(line_str);
            std::deque<std::string> line_words;
            boost::algorithm::split(line_words, line_str, boost::is_any_of("\t \r\n"));

            static char const *const help = "Help:\n"
                                            "    sum <integer>...            Addition\n"
                                            "    sub <integer>...            Substraction\n"
                                            "    mul <integer>...            Multiplication\n"
                                            "    div <integer>...            Division\n"
                                            "    mod <integer> <integer>     Remainder\n"
                                            "    <anything else>             This message";

            if (line_words.empty()) {
                return help;
            }
            std::string command = line_words.front();
            line_words.pop_front();

            if (command == "sum") {
                std::size_t sum = 0;
                for (auto const &w : line_words) {
                    sum += get_integer(w);
                }
                return std::to_string(sum);
            } else if (command == "sub") {
                if (line_words.size() < 2) {
                    return "sub: at least two integers are expected";
                }
                std::size_t sub = get_integer(line_words[0]);
                line_words.pop_front();
                for (auto const &w : line_words) {
                    sub -= get_integer(w);
                }
                return std::to_string(sub);
            } else if (command == "mul") {
                std::size_t mul = 1;
                for (auto const &w : line_words) {
                    mul *= get_integer(w);
                }
                return std::to_string(mul);
            } else if (command == "div") {
                return "todo";
            } else if (command == "mod") {
                return "todo";
            } else {
                return help;
            }
        }

        static long long get_integer(std::string const &word) {
            long long value = 0;
            std::from_chars(word.data(), word.data() + word.size(), value, 10);
            return value;
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

        auto const address{net::ip::make_address(argv[1])};                            // NOLINT
        auto const port{static_cast<std::uint16_t>(std::strtol(argv[2], nullptr, 0))}; // NOLINT
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
        std::cout << "Server: Starting on: " << endpoint << std::endl;
        net::ip::tcp::acceptor acceptor{io_context, endpoint};
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

    } catch (std::exception const &e) {
        std::cerr << "Server error: " << e.what() << std::endl;
    }
    return 0;
}
