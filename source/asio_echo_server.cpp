// Composing asynchronous operations up to an echo server that gracefully shuts down on CTRL-C.
//
// These are the main aspects addressed by this example:
// - composing asynchronous operations using lambdas for the completion handlers
//      Lambda completion functions are bound to the executor and allocator associated with the final completion
//      handler.
// - multi-chain [*] composed operations
//      Their implementation has multiple outstanding asynchronous operations at the same time which means
//      the final completion handler can be called only when the last of them completes.
// - stopping these multi-chain composed operations
//      As a result of an error or a graceful-stop signal, each such operation stops its internal operations,
//      ignores any subsequent errors that they might report and ensures no new operations are initiated.
//      When all the pending internal operations have completed, the final completion handler will be called.
//      This example shows how a timer object or waiting for a signal can be used as graceful-stop signals.
//
//  [*] the term `chain` is borrowed from here:
//  https://www.boost.org/doc/libs/1_69_0/doc/html/boost_asio/overview/core/strands.html
//  "a [...] chain of asynchronous operations associated with a connection"
//
// The operations implemented in this example are:
// - read data from a socket and write it back periodically until an error occurs (see: `async_repeat_echo`)
// - run a server that accepts clients and runs `async_repeat_echo()` for each of them until the server operation
//      is stopped via a timer object passed by the caller (see: `async_echo_server`)
// - runs the server operation until the SIGINT signal (CTRL-C) is received (see `async_echo_server_until_ctrl_c`)
// - runs the server using an allocator and a strand (see: `async_echo_server_until_ctrl_c_allocator_strand').
//      This is needed because we cannot bind an executor and allocator directly to the `use_future` completion token.
//
// The top level server implementation runs the server composed operation in a thread pool
// with the `use_future` completion token (see `run_server`). It is run side by side with a number of clients
// until a predefined run duration expires and the SIGINT signal (Ctrl-C) is raised (see `run_server_and_clients`)
//
// The implementation of the composed asynchronous operations uses a `shared_async_state` (see shared_async_state.hpp)
// base class which offers:
// - completion handler boilerplate for composed operations, similarly to `boost::beast::stable_async_op_base`
//      https://github.com/boostorg/beast/blob/develop/include/boost/beast/core/async_op_base.hpp
// - support for creating completion handlers from lambda functions
//      see: `shared_async_state::wrap`
// - shared ownership of the internal operation state data, similarly to `shared_handler_storage` from
//      https://gist.github.com/djarek/7994948863f5c5cec4054976b68ba847#file-with_timeout-cpp-L30
// - trying to invoke the final completion handler only when there is a single owner of the state holding it
//      see: `shared_async_state::try_invoke_move_args`
// - a debug utility for checking that completion handlers do not execute concurrently where this is not supported
//      see: `shared_async_state::debug_check_not_concurrent`
//
//#define ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG
#define ASYNC_UTILS_ENABLE_DEBUG_CHECK_NOT_CONCURRENT
#define ASYNC_UTILS_ENABLE_DEBUG_DELAYS
//#define BOOST_ASIO_ENABLE_HANDLER_TRACKING

#include "bind_allocator.hpp"
#include "on_scope_exit.hpp"
#include "shared_async_state.hpp"
#include "stack_handler_allocator.hpp"

#include <atomic>
#include <boost/asio/async_result.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/format.hpp>
#include <boost/interprocess/streams/vectorstream.hpp>
#include <cassert>
#include <chrono>
#include <iostream>
#include <list>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
#include <csignal>
#endif

namespace asio = boost::asio;
using error_code = boost::system::error_code;

// Continuously write on a socket what was read last until an error occurs.
// The completion handler will receive the error that led to the operation being stopped, if any, and
// the total number of bytes that were written on the socket.
// Note: If an error occurs, the socket will be closed.
//
// This is a multi-chain composed operations as it may have multiple outstanding asynchronous operations simultaneously.
// But, as this operation does not support true parallelism - multiple completion handlers executing simultaneously -
// the caller is responsible for ensuring that this initiation call and the completion handlers of all the internal
// operations are running from the same implicit or explicit strand.
//
// This operation is based on these two examples:
// - `async_write_messages()` from
//      https://github.com/boostorg/asio/blob/develop/example/cpp11/operations/composed_5.cpp#L36
// - `async_echo()` from
//      https://github.com/boostorg/beast/blob/develop/example/echo-op/echo_op.cpp#L72
//
template <typename StreamSocket, typename CompletionToken>
auto async_repeat_echo(StreamSocket &socket, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using signature = void(error_code, std::size_t);
    using executor_type = typename StreamSocket::executor_type;
    using handler_traits = async_utils::handler_traits<signature, CompletionToken, executor_type>;
    using buffer_allocator_type = typename handler_traits::template rebound_allocator_type<char>;
    using buffer_t = std::vector<char, buffer_allocator_type>;

    struct state_data {
        state_data(StreamSocket &socket, buffer_allocator_type const &allocator)
            : socket{socket}, reading_buffer{max_size, allocator}, read_buffer{allocator}, writing_buffer{allocator},
              write_timer{socket.get_executor().context()}, is_open{}, total_write_size{} {
            read_buffer.reserve(max_size);
            writing_buffer.reserve(max_size);
        }

        StreamSocket &socket;
        std::size_t const max_size = 128;
        buffer_t reading_buffer;        // buffer for reading in-progress
        buffer_t read_buffer;           // buffer for data that was last read and should be written next
        buffer_t writing_buffer;        // buffer for writing in-progress
        asio::steady_timer write_timer; // periodic write timer
        bool is_open;                   // opened (not stopped) flag
        error_code op_error;            // operation completion error; reported to the final compeltion handler
        std::size_t total_write_size;   // bytes sent; reported to the final compeltion handler
    };

    using shared_async_state = async_utils::shared_async_state<signature, CompletionToken, executor_type, state_data>;
    struct internal_op : shared_async_state {
        using shared_async_state::debug_check_not_concurrent;
        using shared_async_state::wrap; // MSVC workaround (`this->` fails to compile in lambdas that copy `this`)
        state_data &data;

        internal_op(StreamSocket &socket, buffer_allocator_type const &allocator,
                    typename shared_async_state::handler_type &&handler)
            : shared_async_state{socket.get_executor(), std::move(handler), socket, allocator},
              data{shared_async_state::get_data()} {
            DEBUG_CHECK_NOT_CONCURRENT();

            // Start reading and the periodic write back
            // Note: By having both `socket.async_read_some` and `write_timer.async_wait` outstanding at the same time
            // we ensure that if the remote side closes the connection, we detect this immediately.
            // And to serve as a multi-chain composed operation example.
            start_read();
            start_wait_write();
            data.is_open = true;
        }

        void start_read() {
            data.socket.async_read_some(asio::buffer(data.reading_buffer),
                                        wrap([*this](error_code ec, std::size_t bytes) mutable {
                                            DEBUG_CHECK_NOT_CONCURRENT();
                                            if (is_open() && !ec) {
                                                data.reading_buffer.resize(bytes);
                                                std::swap(data.reading_buffer, data.read_buffer);
                                                data.reading_buffer.resize(data.max_size);
                                                start_read();
                                            } else {
                                                stop(ec);
                                            }
                                        }));
        }

        void start_wait_write() {
            data.write_timer.expires_after(std::chrono::seconds{1});
            data.write_timer.async_wait(wrap([*this](error_code ec) mutable {
                DEBUG_CHECK_NOT_CONCURRENT();
                if (is_open() && !ec) {
                    if (!data.read_buffer.empty()) {
                        std::swap(data.read_buffer, data.writing_buffer);
                        data.read_buffer.resize(0);
                    }
                    if (!data.writing_buffer.empty()) {
                        asio::async_write(data.socket, asio::buffer(data.writing_buffer),
                                          wrap([*this](error_code ec, std::size_t bytes) mutable {
                                              DEBUG_CHECK_NOT_CONCURRENT();
                                              if (is_open() && !ec) {
                                                  data.total_write_size += bytes;
                                                  start_wait_write();
                                              } else {
                                                  stop(ec);
                                              }
                                          }));
                    } else {
                        start_wait_write();
                    }
                } else {
                    stop(ec);
                }
            }));
        }

        bool is_open() const { return data.is_open; }
        void stop(error_code ec) {
            if (is_open()) {
                data.op_error = ec;
                data.is_open = false;
                error_code ignored;
                data.socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
                data.socket.close(ignored);
                data.write_timer.cancel();
            }
            try_invoke();
        }
        void try_invoke() { this->try_invoke_move_args(data.op_error, data.total_write_size); }
    };

    typename internal_op::completion_type completion(token);
    auto allocator =
        handler_traits::template get_handler_allocator<typename buffer_t::value_type>(completion.completion_handler);
    internal_op op{socket, allocator, std::move(completion.completion_handler)};
    return completion.result.get();
}

// Runs an echo server until `async_wait()` on `stop_timer` returns.
// The completion handler will receive the error that led to the server being stopped, if any, and the total
// number of clients that connected to it.
// Note: `stop_timer.async_wait()` completing with `asio::error::operation_aborted` is not an error, it is considered
// the normal way of stopping the server execution.
//
// The caller is responsible for ensuring that this initiation call and the completion handlers of all the internal
// operations (not running in their own strand) are running from the same implicit or explicit strand.
template <typename Acceptor, typename CompletionToken>
auto async_echo_server(Acceptor &acceptor, asio::steady_timer &stop_timer, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using signature = void(error_code, std::size_t);
    using executor_type = typename Acceptor::executor_type;
    using socket_type = typename Acceptor::protocol_type::socket;
    using strand_type = asio::strand<executor_type>;

    struct state_data {
        state_data(Acceptor &acceptor, asio::steady_timer &stop_timer)
            : acceptor{acceptor}, stop_timer{stop_timer}, is_open{}, client_count{} {}

        Acceptor &acceptor;
        asio::steady_timer &stop_timer;
        std::list<std::tuple<socket_type, strand_type, std::size_t>> clients;
        bool is_open;
        error_code op_error;
        std::size_t client_count;
    };

    using shared_async_state = async_utils::shared_async_state<signature, CompletionToken, executor_type, state_data>;
    struct internal_op : shared_async_state {
        using shared_async_state::debug_check_not_concurrent;
        using shared_async_state::wrap;
        state_data &data;

        internal_op(Acceptor &acceptor, asio::steady_timer &stop_timer,
                    typename shared_async_state::handler_type &&handler)
            : shared_async_state{acceptor.get_executor(), std::move(handler), acceptor, stop_timer},
              data{shared_async_state::get_data()} {
            DEBUG_CHECK_NOT_CONCURRENT();

            // Waiting for the signal to stop this - runs in the caller's strand
            data.stop_timer.async_wait(wrap([*this](error_code ec) mutable {
                DEBUG_CHECK_NOT_CONCURRENT();
                stop(ec == asio::error::operation_aborted ? error_code() : ec);
            }));
            // Start accepting - runs in the caller's strand
            start_accept();
            data.is_open = true;
        }

        void start_accept() {
            data.acceptor.async_accept(wrap([*this](error_code ec, socket_type socket) mutable {
                DEBUG_CHECK_NOT_CONCURRENT();
                if (is_open() && !ec) {
                    start_accept();
                    start_client(std::move(socket));
                } else {
                    stop(ec);
                }
            }));
        }

        void start_client(socket_type &&socket) {
            error_code ec;
            auto ep = socket.remote_endpoint(ec);
            if (ec) {
                return; // The connection is gone already.
            }

            auto id = data.client_count++;
            data.clients.emplace_back(std::move(socket), data.acceptor.get_executor(), id);
            auto iter = data.clients.end();
            --iter;
            std::cout << boost::format("Server: Client[%1%]: Connected: remote-endpint: %2%") % id % ep << std::endl;
            // Run the client operation through its own strand
            strand_type &client_strand = std::get<1>(*iter);
            asio::dispatch(wrap(client_strand, [*this, iter, &client_strand]() mutable {
                async_repeat_echo(std::get<0>(*iter),
                                  wrap(client_strand, [*this, iter](error_code ec, std::size_t bytes) mutable {
                                      std::cout << boost::format("Server: Client[%1%]: Disconnected: transferred: %2% "
                                                                 "(closing error: %3%:%4%)") %
                                                       std::get<2>(*iter) % bytes % ec % ec.message()
                                                << std::endl;
                                      // As the server doesn't hold a mutex,
                                      // we must update the client list under the server's caller strand.
                                      // Note: `asio::post` is needed as `dispatch` may execute here and then we don't
                                      // get to invoke the handler due to the copy of `this` still on the stack
                                      // (if this client finishing its execution is the last operation of the server).
                                      asio::post(wrap([*this, iter]() mutable {
                                          DEBUG_CHECK_NOT_CONCURRENT();
                                          data.clients.erase(iter);
                                          try_invoke();
                                      }));
                                  }));
            }));
        }

        bool is_open() const { return data.is_open; }
        void stop(error_code ec) {
            if (is_open()) {
                std::cout << boost::format("Server: Stopping (closing error: %1%:%2%)") % ec % ec.message()
                          << std::endl;
                data.op_error = ec;
                data.is_open = false;
                error_code ignored;
                data.acceptor.cancel(ignored);
                for (auto &c : data.clients) {
                    std::get<0>(c).shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
                    std::get<0>(c).close(ignored);
                }
            }
            try_invoke();
        }
        void try_invoke() { this->try_invoke_move_args(data.op_error, data.client_count); }
    };

    typename internal_op::completion_type completion(token);
    internal_op op{acceptor, stop_timer, std::move(completion.completion_handler)};
    return completion.result.get();
}

// Runs the echo server until CTRL-C.
// The completion handler will receive the error that stopped the server, if any, and the total number of clients that
// connected to it.
//
// The caller is responsible for ensuring that this initiation call and the completion handlers of all the internal
// operations (not running in their own strand) are running from the same implicit or explicit strand.
template <typename Acceptor, typename CompletionToken>
auto async_echo_server_until_ctrl_c(Acceptor &acceptor, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using signature = void(error_code, std::size_t);
    using executor_type = typename Acceptor::executor_type;

    struct state_data {
        explicit state_data(Acceptor &acceptor)
            : signals{acceptor.get_executor().context(), SIGINT},
              stop_timer{acceptor.get_executor().context()}, is_open{}, client_count{} {}

        asio::signal_set signals;
        asio::steady_timer stop_timer;
        bool is_open;
        error_code op_error;
        std::size_t client_count;
    };
    using shared_async_state = async_utils::shared_async_state<signature, CompletionToken, executor_type, state_data>;
    struct internal_op : shared_async_state {
        using shared_async_state::debug_check_not_concurrent;
        using shared_async_state::wrap;
        state_data &data;

        internal_op(Acceptor &acceptor, typename shared_async_state::handler_type &&handler)
            : shared_async_state{acceptor.get_executor(), std::move(handler), acceptor},
              data{shared_async_state::get_data()} {
            DEBUG_CHECK_NOT_CONCURRENT();

            data.stop_timer.expires_at(asio::steady_timer::time_point::max());
            data.signals.async_wait(wrap([*this](error_code ec, int /*unused*/) mutable {
                DEBUG_CHECK_NOT_CONCURRENT();
                if (!ec) {
                    std::cout << "\nCTRL-C detected" << std::endl;
                }
                stop(ec);
            }));
            async_echo_server(acceptor, data.stop_timer, wrap([*this](error_code ec, std::size_t client_count) mutable {
                                  DEBUG_CHECK_NOT_CONCURRENT();
                                  data.client_count = client_count;
                                  stop(ec);
                              }));
            data.is_open = true;
        }

        bool is_open() const { return data.is_open; }
        void stop(error_code ec) {
            if (is_open()) {
                data.op_error = ec;
                data.is_open = false;
                data.stop_timer.cancel();
                error_code ignored;
                data.signals.cancel(ignored);
            }
            try_invoke();
        }
        void try_invoke() { this->try_invoke_move_args(data.op_error, data.client_count); }
    };

    typename internal_op::completion_type completion(token);
    internal_op op{acceptor, std::move(completion.completion_handler)};
    return completion.result.get();
}

// Runs the echo server until CTRL-C with an allocator and a strand.
template <typename Acceptor, typename Allocator, typename CompletionToken>
auto async_echo_server_until_ctrl_c_allocator_strand(Acceptor &acceptor, Allocator const &allocator,
                                                     CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using signature = void(error_code, std::size_t);
    using executor_type = typename Acceptor::executor_type;
    struct state_data {
        state_data(executor_type const &executor) : strand(executor) {}
        asio::strand<executor_type> strand;
    };

    using shared_async_state = async_utils::shared_async_state<signature, CompletionToken, executor_type, state_data>;
    struct internal_op : shared_async_state {
        using shared_async_state::debug_check_not_concurrent;
        using shared_async_state::invoke;
        using shared_async_state::wrap;
        state_data &data;

        internal_op(Acceptor &acceptor, Allocator const &allocator, typename shared_async_state::handler_type &&handler)
            : shared_async_state{acceptor.get_executor(), std::move(handler), acceptor.get_executor()},
              data{shared_async_state::get_data()} {

            // Run the server through its own strand
            asio::dispatch(wrap(data.strand, [*this, &acceptor, allocator]() mutable {
                DEBUG_CHECK_NOT_CONCURRENT();
                async_echo_server_until_ctrl_c(
                    acceptor,
                    async_utils::bind_allocator(
                        allocator, wrap(data.strand, [*this](error_code ec, std::size_t client_count) mutable {
                            DEBUG_CHECK_NOT_CONCURRENT();
                            invoke(ec, client_count);
                        })));
            }));
        }
    };

    typename internal_op::completion_type completion(token);
    internal_op op{acceptor, allocator, std::move(completion.completion_handler)};
    return completion.result.get();
}

// Runs the server in a thread pool using a `stack_handler_allocator`.
template <typename Acceptor>
void run_server(Acceptor &acceptor, std::size_t server_thread_count, std::vector<std::thread> &threads) {
    auto &io_context = acceptor.get_executor().context();

    async_utils::stack_handler_memory<64> handler_memory;
    async_utils::stack_handler_allocator<void> handler_allocator(handler_memory);

    // Run the server in a thread-pool and we wait for its completion with a future.
    auto threads_io_work = asio::make_work_guard(io_context);
    for (std::size_t i = 0; i < server_thread_count; ++i) {
        threads.emplace_back([&io_context] { io_context.run(); });
    }

    std::future<std::size_t> f =
        async_echo_server_until_ctrl_c_allocator_strand(acceptor, handler_allocator, asio::use_future);
    try {
        auto client_count = f.get();
        std::cout << boost::format("Server: Stopped: client-count: %1%") % client_count << std::endl;
    } catch (std::exception const &e) {
        std::cout << "Server: Run error: " << e.what() << std::endl;
    }

    threads_io_work.reset();
}

// Runs test clients connecting to the server
template <typename Endpoint>
void run_clients(asio::io_context &io_context, Endpoint const &server_endpoint, std::size_t client_thread_count,
                 std::vector<std::thread> &threads) {

    for (std::size_t c = 0; c < client_thread_count; ++c) {
        threads.emplace_back([c, server_endpoint, &io_context] {
            try {
                std::cout << boost::format("Client[%1%]: Started") % c << std::endl;
                asio::basic_stream_socket<typename Endpoint::protocol_type> socket{io_context};
                socket.connect(server_endpoint);
                std::cout << boost::format("Client[%1%]: Connected") % c << std::endl;

                std::size_t message_count = 0;
                boost::interprocess::basic_ovectorstream<std::vector<char>> stream;
                std::vector<char> write_buffer, read_buffer;
                while (true) {
                    stream.rdbuf()->clear();
                    stream << boost::format("message %1% from client %2%") % message_count++ % c;
                    stream.swap_vector(write_buffer);
                    asio::write(socket, asio::buffer(write_buffer));
                    std::cout << boost::format("Client[%1%]: Sent: %2%") % c %
                                     std::string_view(write_buffer.data(), write_buffer.size())
                              << std::endl;
                    read_buffer.resize(write_buffer.size());
                    for (std::size_t r = 0; r <= c; ++r) {
                        asio::read(socket, asio::buffer(read_buffer));
                        std::cout << boost::format("Client[%1%]: Received: %2%") % c %
                                         std::string_view(read_buffer.data(), read_buffer.size())
                                  << std::endl;
                    }
                }

            } catch (std::exception const &e) {
                std::cout << boost::format("Client[%1%]: Error: %2%") % c % e.what() << std::endl;
            }
        });
    }
}

// Runs a server and its clients.
template <typename Endpoint, typename Duration>
void run_server_and_clients(Endpoint const &server_endpoint, std::size_t server_thread_count,
                            std::size_t client_thread_count, Duration run_duration) {

    std::vector<std::thread> threads;
    threads.reserve(1 + client_thread_count + server_thread_count);
    ON_SCOPE_EXIT([&threads] {
        for (auto &t : threads) {
            BOOST_ASSERT(t.joinable());
            t.join();
        }
    });

    // 1st thread will signal CTRL-C after a while
    threads.emplace_back([&] {
        std::this_thread::sleep_for(run_duration);
        ::raise(SIGINT);
    });

    asio::io_context io_context;
    std::cout << boost::format("Server: Starting on: %1%") % server_endpoint << std::endl;
    asio::basic_socket_acceptor<typename Endpoint::protocol_type> acceptor{io_context, server_endpoint};
    // The client threads can start as the server acceptor is already set up.
    run_clients(io_context, server_endpoint, client_thread_count, threads);
    // And then the server.
    run_server(acceptor, server_thread_count, threads);
}

int main(int argc, char **argv) {
    try {
        std::size_t server_thread_count = 3;
        std::size_t client_thread_count = 20;
        std::uint16_t duration_seconds = 60;
        if (argc > 1) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            duration_seconds = static_cast<std::uint16_t>(std::strtol(argv[1], nullptr, 0));
        }

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
        bool local_sockets = false;
        if (argc > 2) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            local_sockets = (std::strcmp("l", argv[2]) == 0);
        }

        if (local_sockets) {
            char const *test_file = "_TMP_local_server_test";
            std::remove(test_file);
            asio::local::stream_protocol::endpoint ep{test_file};
            run_server_and_clients(ep, server_thread_count, client_thread_count,
                                   std::chrono::seconds{duration_seconds});
            return 0;
        }
#endif
        asio::ip::tcp::endpoint ep{asio::ip::make_address("127.0.0.1"), 8080};
        run_server_and_clients(ep, server_thread_count, client_thread_count, std::chrono::seconds{duration_seconds});
        return 0;
    } catch (std::exception const &e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
    }
    return 1;
}
