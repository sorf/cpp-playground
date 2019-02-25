#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/async_op_base.hpp>
#include <boost/beast/core/buffers_prefix.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
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
template <class AsyncStream, class DynamicBuffer, typename CompletionToken>
auto async_demo_rpc(AsyncStream &stream, DynamicBuffer &buffer, CompletionToken &&token) ->
    typename net::async_result<typename std::decay<CompletionToken>::type, void(error_code)>::return_type {

    static_assert(beast::is_async_stream<AsyncStream>::value, "AsyncStream requirements not met");
    static_assert(net::is_dynamic_buffer<DynamicBuffer>::value, "DynamicBuffer type requirements not met");

    using handler_type = typename net::async_completion<CompletionToken, void(error_code)>::completion_handler_type;
    using base_type = beast::async_op_base<handler_type, beast::executor_type<AsyncStream>>;
    struct internal_op : base_type {
        AsyncStream &m_stream;
        DynamicBuffer &m_buffer;
        std::optional<std::size_t> m_write_size;

        internal_op(AsyncStream &stream, DynamicBuffer &buffer, handler_type &&handler)
            : base_type{std::move(handler), stream.get_executor()}, m_stream(stream), m_buffer(buffer) {
            start_read_some();
        }

        void start_read_some() {
            std::size_t bytes_to_read = 3; // A small value for testing purposes
                                           // (multiple `operator()` calls for most messages)
            m_stream.async_read_some(m_buffer.prepare(bytes_to_read), std::move(*this));
        }

        void operator()(error_code ec, std::size_t bytes_transferred = 0) {
            if (!ec) {
                if (!m_write_size) {
                    // We read something.
                    m_buffer.commit(bytes_transferred);
                    std::size_t pos_nl = find_newline(m_buffer.data());
                    if (pos_nl == 0) {
                        // Read some more until we get a newline
                        return start_read_some();
                    } else {
                        // Write back the line we received.
                        m_write_size = pos_nl;
                        return net::async_write(m_stream, beast::buffers_prefix(pos_nl, m_buffer.data()),
                                                std::move(*this));
                    }
                } else {
                    assert(*m_write_size == bytes_transferred);
                    // We completed a write operation, consume the buffer we have just sent.
                    m_buffer.consume(*m_write_size);
                    // And start reading a new message.
                    m_write_size.reset();
                    return start_read_some();
                }
            }
            // Operation complete here.
            this->invoke_now(ec);
        }

        // Same as:
        // https://github.com/boostorg/beast/blob/c82237512a95487fd67a4287f79f4458ba978f43/example/echo-op/echo_op.cpp#L199
        using const_buffers_type = typename DynamicBuffer::const_buffers_type;
        static std::size_t find_newline(const_buffers_type const &buffers) {
            auto begin = net::buffers_iterator<const_buffers_type>::begin(buffers);
            auto end = net::buffers_iterator<const_buffers_type>::end(buffers);
            auto result = std::find(begin, end, '\n');
            if (result == end) {
                return 0;
            }
            return result + 1 - begin;
        }
    };

    typename net::async_completion<CompletionToken, void(error_code)> init(token);
    internal_op op{stream, buffer, std::move(init.completion_handler)};
    return init.result.get();
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: " << argv[0] << " <address> <port>" << std::endl;
            std::cerr << "Example:\n    " << argv[0] << " 0.0.0.0 8080" << std::endl;
            return -1;
        }

        auto const address{net::ip::make_address(argv[1])};
        auto const port{static_cast<std::uint16_t>(std::strtol(argv[2], nullptr, 0))};
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
        auto socket = acceptor.accept();
        std::cout << "Server: Client connected: " << socket.remote_endpoint() << std::endl;

        // Start the `async_demo_rpc` operation and wait for its completion.
        beast::flat_buffer buffer;
        std::future<void> f = async_demo_rpc(socket, buffer, net::use_future);
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
