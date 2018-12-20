#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/type_traits.hpp>
#include <iostream>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;

namespace {

using error_code = boost::system::error_code;

// A composed operations that reads a message from a socket stream and writes it back.
// It is based on:
// https://github.com/chriskohlhoff/asio/blob/master/asio/src/examples/cpp11/operations/composed_5.cpp
// https://github.com/boostorg/beast/blob/develop/example/echo-op/echo_op.cpp
//
// but attempts an implementation that uses lambda expressions for the internal completion handlers.
template <typename StreamSocket, typename CompletionToken>
auto async_echo_rw(StreamSocket &socket, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using completion_handler_sig = void(error_code, std::size_t);
    using completion_type = asio::async_completion<CompletionToken, completion_handler_sig>;
    using completion_handler_type = typename completion_type::completion_handler_type;

    struct internal_state {
        explicit internal_state(StreamSocket &socket, completion_handler_type &&user_completion_handler)
            : socket(socket), user_completion_handler(std::move(user_completion_handler)),
              io_work{asio::make_work_guard(socket.get_executor())}, echo_buffer(128 /*TODO: Use handler allocator*/) {}
        internal_state(internal_state const &) = delete;
        internal_state(internal_state &&) = default;
        internal_state &operator=(internal_state const &) = delete;
        internal_state &operator=(internal_state &&) = default;

        // clang-format off
        static void async_echo(internal_state&& self) {
            auto read_buffer = asio::buffer(self.echo_buffer);
            self.socket.async_read_some(
                read_buffer,
                asio::bind_executor(
                    self.get_executor(),
                    [self = std::move(self)](error_code ec, std::size_t bytes) mutable {
                        if (!ec) {
                            auto write_buffer = asio::buffer(self.echo_buffer.data(), bytes);
                            asio::async_write(
                                self.socket,
                                write_buffer,
                                asio::bind_executor(
                                    self.get_executor(),
                                    [self = std::move(self), bytes](error_code ec, std::size_t) mutable {
                                        self.call_handler(ec, bytes);
                                    }));
                        } else {
                            self.call_handler(ec, bytes);
                        }
                    }));
        }
        // clang-format on

        void call_handler(error_code ec, std::size_t bytes) {
            io_work.reset();
            // TODO: Deallocate the echo_buffer (especially when using the handler allocator)
            user_completion_handler(ec, bytes);
        }

        using executor_type =
            asio::associated_executor_t<completion_handler_type, typename StreamSocket::executor_type>;
        executor_type get_executor() const noexcept {
            return asio::get_associated_executor(user_completion_handler, socket.get_executor());
        }

        using allocator_type = asio::associated_allocator_t<completion_handler_type, std::allocator<void>>;
        allocator_type get_allocator() const noexcept {
            return asio::get_associated_allocator(user_completion_handler, std::allocator<void>{});
        }

        StreamSocket &socket;
        completion_handler_type user_completion_handler;
        asio::executor_work_guard<typename StreamSocket::executor_type> io_work;
        std::vector<char> echo_buffer;
    };

    static_assert(beast::is_async_stream<StreamSocket>::value, "AsyncStream requirements not met");

    completion_type completion(token);
    internal_state::async_echo(internal_state(socket, std::move(completion.completion_handler)));
    return completion.result.get();
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: echo-op <address> <port>\n"
                  << "Example:\n"
                  << "    echo-op 0.0.0.0 8080\n";
        return EXIT_FAILURE;
    }
    auto const address{asio::ip::make_address(argv[1])};
    auto const port{static_cast<unsigned short>(std::atoi(argv[2]))};

    asio::io_context io_context;
    asio::ip::tcp::socket socket{io_context};
    asio::ip::tcp::acceptor acceptor{io_context};
    asio::ip::tcp::endpoint ep{address, port};
    acceptor.open(ep.protocol());
    acceptor.set_option(asio::socket_base::reuse_address(true));
    acceptor.bind(ep);
    acceptor.listen();
    acceptor.accept(socket);
    async_echo_rw(socket, [&](error_code ec, std::size_t bytes) {
        if (ec) {
            std::cerr << argv[0] << ": " << ec.message() << std::endl;
        } else {
            std::cout << argv[0] << ": transferred: " << bytes << std::endl;
        }
    });
    io_context.run();
    return 0;
}
