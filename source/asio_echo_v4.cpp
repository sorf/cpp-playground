#include "async_state.hpp"

#include <boost/asio/async_result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/type_traits.hpp>
#include <boost/predef.h>
#include <iostream>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;

namespace {

using error_code = boost::system::error_code;

// A composed operations that reads a message from a socket stream and writes it back.
template <typename StreamSocket, typename CompletionToken>
auto async_echo_rw(StreamSocket &socket, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    struct state_data {
        state_data(StreamSocket &socket, std::size_t buffer_size) : socket{socket}, echo_buffer(buffer_size, '\0') {}
        StreamSocket &socket;
        std::vector<char> echo_buffer;
    };

    using state_type = async_utils::async_state<void(error_code, std::size_t), CompletionToken,
                                                typename StreamSocket::executor_type, state_data>;

    static_assert(beast::is_async_stream<StreamSocket>::value, "AsyncStream requirements not met");
    typename state_type::completion_type completion(token);

    state_type state{std::move(completion.completion_handler), socket.get_executor(), socket, 128};
    auto *data = state.get();
    data->socket.async_read_some(
        asio::buffer(data->echo_buffer),
        state.wrap()([=, state = std::move(state)](error_code ec, std::size_t bytes) mutable {
            if (!ec) {
                asio::async_write(data->socket, asio::buffer(data->echo_buffer.data(), bytes),
                                  state.wrap()([=, state = std::move(state)](error_code ec, std::size_t) mutable {
                                      state.invoke(ec, bytes);
                                  }));
            } else {
                state.invoke(ec, bytes);
            }
        }));
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
    char const *arg_program = argv[0]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    char const *arg_address = argv[1]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    char const *arg_port = argv[2];    // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    try {
        auto const address{asio::ip::make_address(arg_address)};
        auto const port{static_cast<std::uint16_t>(std::strtol(arg_port, nullptr, 0))};

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
                std::cerr << arg_program << ": " << ec.message() << std::endl;
            } else {
                std::cout << arg_program << ": transferred: " << bytes << std::endl;
            }
        });
        io_context.run();
    } catch (std::exception const &e) {
        std::cerr << arg_program << ": error: " << e.what() << std::endl;
    }
    return 0;
}
