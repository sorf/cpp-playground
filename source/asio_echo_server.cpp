//#define ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG

#include "bind_allocator.hpp"
#include "on_scope_exit.hpp"
#include "shared_async_state.hpp"
#include "stack_handler_allocator.hpp"

#include <atomic>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/format.hpp>
#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>

namespace asio = boost::asio;
using error_code = boost::system::error_code;

// Helper base class to add a 'continue_handler' method in derived classes.
template <class Derived> struct add_continue_handler {

    // Tests if am intermediary completion handler should continue its execution.
    // It shouldn't if the intermediary operation has failed or if the composed operation has been closed.
    // In this case the final completion handler is called, if this is the last instance sharing the state owernship.
    bool continue_handler(error_code ec) {
        auto *pthis = static_cast<Derived *>(this);
        if (pthis->is_open() && !ec) {
            return true;
        }

        if (pthis->is_open()) {
            pthis->close(ec);
        }
        pthis->try_invoke();
        return false;
    }
};

// Continuously write on a socket what was read last, until async_wait() on the close timer returns.
// The completion handler will receive the error that led to the connection being closed and
// the total number of bytes that were written on the socket.
template <typename StreamSocket, typename CompletionToken>
auto async_repeat_echo(StreamSocket &socket, asio::steady_timer &close_timer, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using signature = void(error_code, std::size_t);
    using buffer_allocator = async_utils::define_handler_allocator_token<char, signature, CompletionToken>;
    using buffer_t = std::vector<char, buffer_allocator>;

    struct state_data {
        state_data(StreamSocket &socket, asio::steady_timer &close_timer, buffer_allocator const &allocator)
            : socket{socket}, close_timer{close_timer}, reading_buffer{max_size, allocator},
              read_buffer{max_size, allocator}, writing_buffer{max_size, allocator},
              write_timer{close_timer.get_executor().context()}, total_write_size{}, is_open{} {}

        StreamSocket &socket;
        asio::steady_timer &close_timer;
        std::size_t const max_size = 128;
        buffer_t reading_buffer;
        buffer_t read_buffer;
        buffer_t writing_buffer;
        asio::steady_timer write_timer;
        std::size_t total_write_size;
        error_code user_completion_error;
        bool is_open;
    };

    using base_type =
        async_utils::shared_async_state<signature, CompletionToken, asio::io_context::executor_type, state_data>;
    struct internal_op : base_type, add_continue_handler<internal_op> {
        using base_type::wrap; // MSVC workaround ('this->` fails to compile in lambdas that copy 'this')
        using add_continue_handler<internal_op>::continue_handler;
        state_data &data;

        internal_op(StreamSocket &socket, asio::steady_timer &close_timer, buffer_allocator const &allocator,
                    typename base_type::completion_handler_type &&completion_handler)
            : base_type{socket.get_executor(), std::move(completion_handler), socket, close_timer, allocator},
              data{base_type::get_data()} {

            // Waiting for the signal to close this (with either the async_wait error or operation_aborted)
            data.close_timer.async_wait(wrap([*this](error_code ec) mutable {
                if (continue_handler(ec)) {
                    close(asio::error::operation_aborted);
                }
            }));
            // Start reading and the periodic write back
            start_read();
            start_wait_write();
            data.is_open = true;
        }

        void start_read() {
            data.socket.async_read_some(asio::buffer(data.reading_buffer),
                                        wrap([*this](error_code ec, std::size_t bytes) mutable {
                                            if (continue_handler(ec)) {
                                                data.reading_buffer.resize(bytes);
                                                std::swap(data.reading_buffer, data.read_buffer);
                                                data.reading_buffer.resize(data.max_size);
                                                start_read();
                                            }
                                        }));
        }

        void start_wait_write() {
            data.write_timer.expires_after(std::chrono::seconds(1));
            data.write_timer.async_wait(wrap([*this](error_code ec) mutable {
                if (continue_handler(ec)) {
                    if (!data.read_buffer.empty()) {
                        std::swap(data.read_buffer, data.writing_buffer);
                        data.read_buffer.resize(0);
                    }
                    if (!data.writing_buffer.empty()) {
                        asio::async_write(data.socket, asio::buffer(data.writing_buffer),
                                          wrap([*this](error_code ec, std::size_t bytes) mutable {
                                              if (continue_handler(ec)) {
                                                  data.total_write_size += bytes;
                                                  start_wait_write();
                                              }
                                          }));
                    } else {
                        start_wait_write();
                    }
                }
            }));
        }

        bool is_open() const { return data.is_open; }
        void close(error_code ec) {
            data.user_completion_error = ec;
            data.is_open = false;
            error_code ignored;
            data.socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
            data.socket.close(ignored);
            data.close_timer.cancel();
            data.write_timer.cancel();
        }
        void try_invoke() { this->try_invoke_move_args(data.user_completion_error, data.total_write_size); }
    };

    typename internal_op::completion_type completion(token);
    auto allocator = async_utils::get_handler_allocator<typename buffer_t::value_type>(completion.completion_handler);
    internal_op op{socket, close_timer, allocator, std::move(completion.completion_handler)};
    return completion.result.get();
}

// Runs an echo server.
// The completion handler will receive the error that closed the server and the total number of clients that
// connected to it.
template <typename Acceptor, typename CompletionToken>
auto async_echo_server(Acceptor &acceptor, asio::steady_timer &close_timer, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using signature = void(error_code, std::size_t);
    using socket_type = typename Acceptor::protocol_type::socket;
    using endpoint_type = typename Acceptor::endpoint_type;

    struct state_data {
        state_data(Acceptor &acceptor, asio::steady_timer &close_timer)
            : acceptor{acceptor}, close_timer{close_timer}, total_client_count{}, is_open{} {}

        Acceptor &acceptor;
        asio::steady_timer &close_timer;
        std::map<endpoint_type, std::tuple<socket_type, asio::steady_timer>> clients;
        std::size_t total_client_count;
        error_code user_completion_error;
        bool is_open;
    };

    using base_type =
        async_utils::shared_async_state<signature, CompletionToken, asio::io_context::executor_type, state_data>;
    struct internal_op : base_type, add_continue_handler<internal_op> {
        using base_type::wrap;
        using add_continue_handler<internal_op>::continue_handler;
        state_data &data;

        internal_op(Acceptor &acceptor, asio::steady_timer &close_timer,
                    typename base_type::completion_handler_type &&completion_handler)
            : base_type{acceptor.get_executor(), std::move(completion_handler), acceptor, close_timer},
              data{base_type::get_data()} {

            // Waiting for the signal to close this (with either the async_wait error or operation_aborted)
            data.close_timer.async_wait(wrap([*this](error_code ec) mutable {
                if (continue_handler(ec)) {
                    close(asio::error::operation_aborted);
                }
            }));
            // Start accepting
            start_accept();
            data.is_open = true;
        }

        void start_accept() {
            data.acceptor.async_accept(wrap([*this](error_code ec, socket_type socket) mutable {
                if (continue_handler(ec)) {
                    start_accept();
                    start_client(std::move(socket));
                }
            }));
        }

        void start_client(socket_type &&socket) {
            asio::steady_timer close_timer{data.close_timer.get_executor().context()};
            close_timer.expires_at(asio::steady_timer::time_point::max());
            error_code ec;
            auto endpoint = socket.remote_endpoint(ec);
            if (ec) {
                return; // The connection is gone already.
            }

            ++data.total_client_count;
            auto i = data.clients.try_emplace(std::move(endpoint), std::move(socket), std::move(close_timer)).first;
            std::cout << boost::format("client[%1%]: connected") % i->first << std::endl;
            async_repeat_echo(
                std::get<0>(i->second), std::get<1>(i->second), [*this, i](error_code ec, std::size_t bytes) mutable {
                    std::cout << boost::format("client[%1%]: disconnected. transferred: %2% (closing error: %3%:%4%)") %
                                     i->first % bytes % ec % ec.message()
                              << std::endl;
                    data.clients.erase(i);
                    try_invoke();
                });
        }

        bool is_open() const { return data.is_open; }
        void close(error_code ec) {
            data.user_completion_error = ec;
            data.is_open = false;
            error_code ignored;
            data.acceptor.close(ignored);
            for (auto &&[k, v] : data.clients) {
                std::get<1>(v).cancel();
            }
        }
        void try_invoke() { this->try_invoke_move_args(data.user_completion_error, data.total_client_count); }
    };

    typename internal_op::completion_type completion(token);
    internal_op op{acceptor, close_timer, std::move(completion.completion_handler)};
    return completion.result.get();
}

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
        asio::steady_timer close_timer{io_context};
        close_timer.expires_at(asio::steady_timer::time_point::max());

        asio::ip::tcp::acceptor acceptor{io_context};
        asio::ip::tcp::endpoint endpoint{address, port};
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();

        asio::signal_set signals{io_context, SIGINT};
        signals.async_wait([&](error_code ec, int /*unused*/) {
            if (ec) {
                std::cout << boost::format("Error waiting for signal: %1%:%2%") % ec % ec.message() << std::endl;
            } else {
                std::cout << "CTRL-C" << std::endl;
            }
            close_timer.cancel();
        });

        std::cout << boost::format("server[%1%]: started") % endpoint << std::endl;
        async_echo_server(acceptor, close_timer, [&](error_code ec, std::size_t client_count) {
            std::cout << boost::format("server[%1%]: stopped. clients: %2% (closing error: %3%:%4%)") % endpoint %
                             client_count % ec % ec.message()
                      << std::endl;
        });

        io_context.run();
    } catch (std::exception const &e) {
        std::cerr << arg_program << ": fatal error: " << e.what() << std::endl;
    }
    return 0;
}
