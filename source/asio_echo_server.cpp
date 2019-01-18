// Composing asynchronous operations up to an echo server that gracefully shuts down on CTRL-C.
//
// This example shows writing and composing asynchronous operations.
// Some of these composed operations are multi-chain (they consist of multiple flows of execution) which means
// that they can have multiple outstanding asynchronous operations simultaneously. This requires that
// the final completion handler has to be called only when the last of them completes.
//
// The operations implemented in this example are:
// - read data from a socket and write it back periodically until an error occurs or the operation is stopped
//      via a timer object passsed by the caller (see: `async_repeat_echo`)
// - run a server that accepts clients and runs `async_repeat_echo()` for each of them, until the the operation
//      is stopped via a timer object passsed by the caller (see: `async_echo_server`)
// - runs a server until the SIGINT signal (CTRL-C) is received (see `async_echo_server_until_ctrl_c`)
// - runs a server until stopped using the given allocator (see: `async_echo_server_until_ctrl_c_allocator').
//      This is needed because we cannot bind an allocator directly to the `use_future` completion token.
//
// There are two top level server implementations, each of them running the server composed operation in a thread pool
// with the `use_future` completion token (see `run_server_and_join`).
// One top level implementation runs a TCP server until the SIGINT signal (Ctrl-C) is received (see `run_tcp_server`).
// The other runs a UNIX domain sockets server for a fixed duration of time while clients connect to it to
// send and receive messages (see `run_unix_local_server_clients`).
//
//
// The implementation of the composed asynchronous operations uses these:
// - a `shared_async_state` (see shared_async_state.hpp) base class similar to `stable_async_op_base`
//      https://github.com/boostorg/beast/blob/develop/include/boost/beast/core/async_op_base.hpp#L559
//      but which offers shared ownership of the internal operation state data,
//      similarly to `shared_handler_storage`
//      https://gist.github.com/djarek/7994948863f5c5cec4054976b68ba847#file-with_timeout-cpp-L30
//      and a way of creating completion handlers from lambda functions.
// - an `add_stop` base class that implements common logic related to the stopping sequence,
//      invoking the final completion handler and detecting concurrent execution of completion handlers where this is
//      not supported.
// - some of them use a `steady_timer` as a way to signal when they should stop.
//      The caller sets up a timer object to never expire for each such operation and then stops it when the operation
//      should stop.
//
//#define ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG
//#define BOOST_ASIO_ENABLE_HANDLER_TRACKING

#include "bind_allocator.hpp"
#include "on_scope_exit.hpp"
#include "shared_async_state.hpp"
#include "stack_handler_allocator.hpp"

#include <atomic>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
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
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
#include <csignal>
#endif

namespace asio = boost::asio;
using error_code = boost::system::error_code;

namespace async_utils {

// Helper base class for the logic around triggering and handling the stopping of asynchronous operations.
// It also adds support for verifying that there are no completion handlers executing simultaneously
// where this is not expected.
//
// The derived class should provide:
//      - std::atomic_bool *Derived::executing() // optional (if it returns nullptr)
//      - bool Derived::is_open()
//      - Derived::stop_impl(error_code)
//      - Derived::try_invoke()
template <class Derived> struct add_stop {
  private:
    template <typename F> struct [[nodiscard]] check_scope_exit {
        explicit check_scope_exit(F && f) : m_f(std::move(f)) {}
        check_scope_exit(check_scope_exit const &) = delete;
        check_scope_exit(check_scope_exit &&) = delete;
        check_scope_exit &operator=(check_scope_exit const &) = delete;
        check_scope_exit &operator=(check_scope_exit &&other) = delete;
        ~check_scope_exit() { reset(); }
        void reset() noexcept {
            if (m_f) {
                (*m_f)();
                m_f.reset();
            }
        }

      private:
        std::optional<F> m_f;
    };

    template <typename F> decltype(auto) make_check_scope_exit(F &&f) {
        return check_scope_exit<F>(std::forward<F>(f));
    }

  public:
    // Returns a check scope guard object that ensures there are no concurrent accesses until its destruction.
    decltype(auto) check_not_concurrent() {
        auto *pthis = static_cast<Derived *>(this);
        std::atomic_bool *executing_ptr = pthis->executing();
        if (executing_ptr != nullptr) {
            bool was_executing = executing_ptr->exchange(true);
            BOOST_ASSERT(!was_executing);
        }

        return make_check_scope_exit([executing_ptr] {
            if (executing_ptr != nullptr) {
                bool was_executing = executing_ptr->exchange(false);
                BOOST_ASSERT(was_executing);
            }
        });
    }

    // Tests if an intermediary completion handler should continue its execution.
    // It shouldn't if the intermediary operation has failed or if the composed operation has been stopped.
    // In these cases, the final completion handler is attempted to be called; it will be called only when
    // this is the last instance sharing the ownership of the composed operation state.
    template <typename CheckScopeExit = std::optional<unsigned>>
    bool continue_handler(error_code ec, CheckScopeExit *check_scope_exit = nullptr) {
        auto *pthis = static_cast<Derived *>(this);
        if (pthis->is_open() && !ec) {
            return true;
        }
        stop(ec, check_scope_exit);
        return false;
    }

    template <typename CheckScopeExit> bool continue_handler(error_code ec, CheckScopeExit &check_scope_exit) {
        return continue_handler(ec, &check_scope_exit);
    }

    // Initiates the stopping of the asynchronous operations initiated by the base class
    // and attempts to call the final completion handler.
    // 
    // Note: The actual implementation of the stopping is in Derived::stop_impl().
    template <typename CheckScopeExit = std::optional<unsigned>>
    void stop(error_code ec, CheckScopeExit *check_scope_exit = nullptr) {
        auto *pthis = static_cast<Derived *>(this);
        if (pthis->is_open()) {
            pthis->stop_impl(ec);
        }
        if (check_scope_exit != nullptr) {
            check_scope_exit->reset();
        }
        pthis->try_invoke();
    }

    template <typename CheckScopeExit> void stop(error_code ec, CheckScopeExit &check_scope_exit) {
        return stop(ec, &check_scope_exit);
    }
};

} // namespace async_utils

// Continuously write on a socket what was read last, until `async_wait()` on `stop_timer` returns.
// The completion handler will receive the error that led to the operation being stopped, if any, and
// the total number of bytes that were written on the socket.
// Note: `stop_timer.async_wait()` completing with `asio::error::operation_aborted` is not an error, it is considered
// the normal way to stop the execution of this operation.
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
auto async_repeat_echo(StreamSocket &socket, asio::steady_timer &stop_timer, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using signature = void(error_code, std::size_t);
    using executor_type = typename StreamSocket::executor_type;
    using handler_traits = async_utils::handler_traits<signature, CompletionToken, executor_type>;
    using buffer_allocator_type = typename handler_traits::template rebound_allocator_type<char>;
    using buffer_t = std::vector<char, buffer_allocator_type>;

    struct state_data {
        state_data(StreamSocket &socket, asio::steady_timer &stop_timer, buffer_allocator_type const &allocator)
            : socket{socket}, stop_timer{stop_timer}, reading_buffer{max_size, allocator}, read_buffer{allocator},
              writing_buffer{allocator}, write_timer{stop_timer.get_executor().context()}, executing{}, is_open{},
              total_write_size{} {
            read_buffer.reserve(max_size);
            writing_buffer.reserve(max_size);
        }

        StreamSocket &socket;
        asio::steady_timer &stop_timer;
        std::size_t const max_size = 128;
        buffer_t reading_buffer;        // buffer for reading in-progress
        buffer_t read_buffer;           // buffer for data that was last read and should be written next
        buffer_t writing_buffer;        // buffer for writing in-progress
        asio::steady_timer write_timer; // periodic write timer
        std::atomic_bool executing;     // flag for detecting concurrent execution of code that is not thread-safe
        bool is_open;                   // opened (not stopped) flag
        error_code op_error;            // operation completion error; reported to the final compeltion handler
        std::size_t total_write_size;   // bytes sent; reported to the final compeltion handler
    };

    using shared_async_state = async_utils::shared_async_state<signature, CompletionToken, executor_type, state_data>;
    struct internal_op : shared_async_state, async_utils::add_stop<internal_op> {
        using shared_async_state::wrap; // MSVC workaround (`this->` fails to compile in lambdas that copy `this`)
        using async_utils::add_stop<internal_op>::check_not_concurrent;
        using async_utils::add_stop<internal_op>::continue_handler;
        using async_utils::add_stop<internal_op>::stop;
        state_data &data;

        internal_op(StreamSocket &socket, asio::steady_timer &stop_timer, buffer_allocator_type const &allocator,
                    typename shared_async_state::handler_type &&handler)
            : shared_async_state{socket.get_executor(), std::move(handler), socket, stop_timer, allocator},
              data{shared_async_state::get_data()} {
            [[maybe_unused]] auto check = check_not_concurrent();

            // Waiting for the signal to stop this
            data.stop_timer.async_wait(wrap([*this](error_code ec) mutable {
                auto check = check_not_concurrent();
                stop(ec == asio::error::operation_aborted ? error_code() : ec, check);
            }));
            // Start reading and the periodic write back
            start_read();
            start_wait_write();
            data.is_open = true;
        }

        void start_read() {
            data.socket.async_read_some(asio::buffer(data.reading_buffer),
                                        wrap([*this](error_code ec, std::size_t bytes) mutable {
                                            auto check = check_not_concurrent();
                                            if (continue_handler(ec, check)) {
                                                data.reading_buffer.resize(bytes);
                                                std::swap(data.reading_buffer, data.read_buffer);
                                                data.reading_buffer.resize(data.max_size);
                                                start_read();
                                            }
                                        }));
        }

        void start_wait_write() {
            data.write_timer.expires_after(std::chrono::seconds{1});
            data.write_timer.async_wait(wrap([*this](error_code ec) mutable {
                auto check = check_not_concurrent();
                if (continue_handler(ec, check)) {
                    if (!data.read_buffer.empty()) {
                        std::swap(data.read_buffer, data.writing_buffer);
                        data.read_buffer.resize(0);
                    }
                    if (!data.writing_buffer.empty()) {
                        asio::async_write(data.socket, asio::buffer(data.writing_buffer),
                                          wrap([*this](error_code ec, std::size_t bytes) mutable {
                                              auto check = check_not_concurrent();
                                              if (continue_handler(ec, check)) {
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

        std::atomic_bool *executing() { return &data.executing; }
        bool is_open() const { return data.is_open; }
        void stop_impl(error_code ec) {
            data.op_error = ec;
            data.is_open = false;
            error_code ignored;
            data.socket.cancel(ignored);
            data.stop_timer.cancel();
            data.write_timer.cancel();
        }
        void try_invoke() {
            BOOST_ASSERT(!data.executing); // This ensures there is no outstanding `check_not_concurrent` scope guard.
            this->try_invoke_move_args(data.op_error, data.total_write_size);
        }
    };

    typename internal_op::completion_type completion(token);
    auto allocator =
        handler_traits::template get_handler_allocator<typename buffer_t::value_type>(completion.completion_handler);
    internal_op op{socket, stop_timer, allocator, std::move(completion.completion_handler)};
    return completion.result.get();
}

// Runs an echo server.
// The completion handler will receive the error that led to the operation being stopped, if any, and the total
// number of clients that connected to it.
// The operation uses strands internally, so there are no requirements on the way it is initiated.
template <typename Acceptor, typename CompletionToken>
auto async_echo_server(Acceptor &acceptor, asio::steady_timer &stop_timer, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using signature = void(error_code, std::size_t);
    using executor_type = typename Acceptor::executor_type;
    using socket_type = typename Acceptor::protocol_type::socket;
    using handler_traits = async_utils::handler_traits<signature, CompletionToken, executor_type>;
    using handler_executor_type = typename handler_traits::handler_executor_type;
    using strand_type = asio::strand<handler_executor_type>;

    struct state_data {
        state_data(Acceptor &acceptor, asio::steady_timer &stop_timer)
            : acceptor{acceptor}, stop_timer{stop_timer}, executing{}, is_open{}, client_count{} {}

        Acceptor &acceptor;
        asio::steady_timer &stop_timer;
        std::list<std::tuple<socket_type, asio::steady_timer, strand_type, std::size_t>> clients;
        std::atomic_bool executing;
        bool is_open;
        error_code op_error;
        std::size_t client_count;
    };

    using shared_async_state = async_utils::shared_async_state<signature, CompletionToken, executor_type, state_data>;
    struct internal_op : shared_async_state, async_utils::add_stop<internal_op> {
        using shared_async_state::wrap;
        using async_utils::add_stop<internal_op>::check_not_concurrent;
        using async_utils::add_stop<internal_op>::continue_handler;
        using async_utils::add_stop<internal_op>::stop;
        state_data &data;
        handler_executor_type handler_executor;
        strand_type server_strand;

        internal_op(Acceptor &acceptor, asio::steady_timer &stop_timer,
                    typename shared_async_state::handler_type &&handler)
            : shared_async_state{acceptor.get_executor(), std::move(handler), acceptor, stop_timer},
              data{shared_async_state::get_data()}, handler_executor{shared_async_state::get_handler_executor()},
              server_strand{handler_executor} {
            // Initiate the operations to run in the server strand
            asio::dispatch(
                asio::bind_executor(server_strand, wrap([*this]() mutable {
                                        [[maybe_unused]] auto check = check_not_concurrent();
                                        // Waiting for the signal to stop this
                                        data.stop_timer.async_wait(asio::bind_executor(
                                            server_strand, wrap([*this](error_code ec) mutable {
                                                auto check = check_not_concurrent();
                                                stop(ec == asio::error::operation_aborted ? error_code() : ec, check);
                                            })));
                                        // Start accepting
                                        start_accept();
                                        data.is_open = true;
                                    })));
        }

        void start_accept() {
            data.acceptor.async_accept(
                asio::bind_executor(server_strand, wrap([*this](error_code ec, socket_type socket) mutable {
                                        auto check = check_not_concurrent();
                                        if (continue_handler(ec, check)) {
                                            start_accept();
                                            start_client(std::move(socket));
                                        }
                                    })));
        }

        void start_client(socket_type &&socket) {
            error_code ec;
            auto ep = socket.remote_endpoint(ec);
            if (ec) {
                return; // The connection is gone already.
            }
            asio::steady_timer stop_timer{data.acceptor.get_executor().context()};
#ifndef __clang_analyzer__ // Silence: bugprone-use-after-move
            stop_timer.expires_at(asio::steady_timer::time_point::max());
#endif
            auto id = data.client_count++;
            data.clients.emplace_back(std::move(socket), std::move(stop_timer), handler_executor, id);
            auto iter = data.clients.end();
            --iter;
            std::cout << boost::format("Server: Client[%1%]: Connected: remote-endpint: %2%") % id % ep << std::endl;
            // Run the client operation through its own strand
            strand_type &client_strand = std::get<2>(*iter);
            asio::dispatch(asio::bind_executor(
                client_strand, wrap([*this, iter, &client_strand]() mutable {
                    async_repeat_echo(
                        std::get<0>(*iter), std::get<1>(*iter),
                        asio::bind_executor(
                            client_strand, wrap([*this, iter](error_code ec, std::size_t bytes) mutable {
                                // Closing the client socket and saving any first error to log it
                                auto &client_socket = std::get<0>(*iter);
                                error_code ignored;
                                client_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, !ec ? ec : ignored);
                                client_socket.close(!ec ? ec : ignored);
                                std::cout << boost::format("Server: Client[%1%]: Disconnected: transferred: %2% "
                                                           "(closing error: %3%:%4%)") %
                                                 std::get<3>(*iter) % bytes % ec % ec.message()
                                          << std::endl;
                                data.clients.erase(iter);
                                // Call the final completion handler if this was the last server operation
                                try_invoke();
                            })));
                })));
        }

        std::atomic_bool *executing() { return &data.executing; }
        bool is_open() const { return data.is_open; }
        void stop_impl(error_code ec) {
            data.op_error = ec;
            data.is_open = false;
            error_code ignored;
            data.acceptor.cancel(ignored);
            for (auto &c : data.clients) {
                std::get<1>(c).cancel();
            }
        }
        void try_invoke() {
            BOOST_ASSERT(!data.executing); // This ensures there is no outstanding `check_not_concurrent` scope guard.
            this->try_invoke_move_args(data.op_error, data.client_count);
        }
    };

    typename internal_op::completion_type completion(token);
    internal_op op{acceptor, stop_timer, std::move(completion.completion_handler)};
    return completion.result.get();
}

// Runs the echo server until CTRL-C.
// The completion handler will receive the error that stopped the server, if any, and the total number of clients that
// connected to it.
// The operation uses strands internally, so there are no requirements on the way it is initiated.
template <typename Acceptor, typename CompletionToken>
auto async_echo_server_until_ctrl_c(Acceptor &acceptor, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using signature = void(error_code, std::size_t);
    using executor_type = typename Acceptor::executor_type;

    struct state_data {
        explicit state_data(Acceptor &acceptor)
            : signals{acceptor.get_executor().context(), SIGINT},
              stop_timer{acceptor.get_executor().context()}, executing{}, is_open{}, client_count{} {}

        asio::signal_set signals;
        asio::steady_timer stop_timer;
        std::atomic_bool executing;
        bool is_open;
        error_code op_error;
        std::size_t client_count;
    };
    using shared_async_state = async_utils::shared_async_state<signature, CompletionToken, executor_type, state_data>;
    struct internal_op : shared_async_state, async_utils::add_stop<internal_op> {
        using shared_async_state::wrap;
        using async_utils::add_stop<internal_op>::check_not_concurrent;
        using async_utils::add_stop<internal_op>::continue_handler;
        using async_utils::add_stop<internal_op>::stop;
        state_data &data;
        typename shared_async_state::handler_executor_type handler_executor;
        asio::strand<typename shared_async_state::handler_executor_type> strand;

        internal_op(Acceptor &acceptor, typename shared_async_state::handler_type &&handler)
            : shared_async_state{acceptor.get_executor(), std::move(handler), acceptor},
              data{shared_async_state::get_data()}, handler_executor{shared_async_state::get_handler_executor()},
              strand{shared_async_state::get_handler_executor()} {

            // We initiate and run the waiting for CTRL-C in the strand of `this`.
            asio::dispatch(asio::bind_executor(strand, wrap([*this]() mutable {
                                                   [[maybe_unused]] auto check = check_not_concurrent();
                                                   data.stop_timer.expires_at(asio::steady_timer::time_point::max());
                                                   data.signals.async_wait(asio::bind_executor(
                                                       strand, wrap([*this](error_code ec, int /*unused*/) mutable {
                                                           auto check = check_not_concurrent();
                                                           if (!ec) {
                                                               std::cout << "\nCTRL-C detected" << std::endl;
                                                           }
                                                           stop(ec, check);
                                                       })));
                                               })));

            // We don't need to initiate the run-server operation in the strand of `this` as it uses strands internally.
            // But the completion handler will have to run in the strand.
            async_echo_server(
                acceptor, data.stop_timer,
                asio::bind_executor(strand, wrap([*this](error_code ec, std::size_t client_count) mutable {
                                        auto check = check_not_concurrent();
                                        data.client_count = client_count;
                                        stop(ec, check);
                                    })));

            data.is_open = true;
        }

        std::atomic_bool *executing() { return &data.executing; }
        bool is_open() const { return data.is_open; }
        void stop_impl(error_code ec) {
            data.op_error = ec;
            data.is_open = false;
            data.stop_timer.cancel();
            error_code ignored;
            data.signals.cancel(ignored);
        }
        void try_invoke() {
            BOOST_ASSERT(!data.executing);
            this->try_invoke_move_args(data.op_error, data.client_count);
        }
    };

    typename internal_op::completion_type completion(token);
    internal_op op{acceptor, std::move(completion.completion_handler)};
    return completion.result.get();
}

// Runs the echo server until CTRL-C with an allocator.
template <typename Acceptor, typename Allocator, typename CompletionToken>
auto async_echo_server_until_ctrl_c_allocator(Acceptor &acceptor, Allocator const &allocator, CompletionToken &&token)
    -> typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using signature = void(error_code, std::size_t);
    using executor_type = typename Acceptor::executor_type;
    struct state_data {};

    using shared_async_state = async_utils::shared_async_state<signature, CompletionToken, executor_type, state_data>;
    struct internal_op : shared_async_state {
        using shared_async_state::invoke; // MSVC workaround (`this->invoke()` fails to compile in the lambda)
        internal_op(Acceptor &acceptor, Allocator const &allocator, typename shared_async_state::handler_type &&handler)
            : shared_async_state{acceptor.get_executor(), std::move(handler)} {

            // We don't need any synchronization here as we start a single asynchronous operation
            // which doesn't have any requirements on the way it is initiated.
            async_echo_server_until_ctrl_c(
                acceptor, async_utils::bind_allocator(
                              allocator, this->wrap([*this](error_code ec, std::size_t client_count) mutable {
                                  invoke(ec, client_count);
                              })));
        }
    };

    typename internal_op::completion_type completion(token);
    internal_op op{acceptor, allocator, std::move(completion.completion_handler)};
    return completion.result.get();
}

// Runs the server in a thread pool using a `stack_handler_allocator`.
// At the end it joins all the threads, even if an exception occurs.
template <typename Acceptor>
auto run_server_and_join(Acceptor &acceptor, std::size_t server_thread_count, std::vector<std::thread> &threads) {

    ON_SCOPE_EXIT([&threads] {
        for (auto &t : threads) {
            BOOST_ASSERT(t.joinable());
            t.join();
        }
    });

    async_utils::stack_handler_memory<64> handler_memory;
    async_utils::stack_handler_allocator<void> handler_allocator(handler_memory);

    // Run the server in a thread-pool and we wait for its completion with a future.
    auto &io_context = acceptor.get_executor().context().get_executor().context();
    auto threads_io_work = asio::make_work_guard(io_context);
    for (std::size_t i = 0; i < server_thread_count; ++i) {
        threads.emplace_back([&io_context] { io_context.run(); });
    }

    std::future<std::size_t> f =
        async_echo_server_until_ctrl_c_allocator(acceptor, handler_allocator, asio::use_future);
    try {
        auto client_count = f.get();
        std::cout << boost::format("Server: Stopped: client-count: %1%") % client_count << std::endl;
    } catch (std::exception const &e) {
        std::cout << "Server: Run error: " << e.what() << std::endl;
    }

    threads_io_work.reset();
}

// Runs a TCP echo server.
int run_tcp_server(std::size_t server_thread_count, int argc, char **argv) {
    char const *arg_program = argv[0]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (argc != 3) {
        std::cerr << "Usage as a TCP server: " << arg_program << " <address> <port>\n"
                  << "Example:\n"
                  << "    " << arg_program << " 0.0.0.0 8080" << std::endl;
        return 1;
    }
    char const *arg_address = argv[1]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    char const *arg_port = argv[2];    // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    auto const address{asio::ip::make_address(arg_address)};
    auto const port{static_cast<std::uint16_t>(std::strtol(arg_port, nullptr, 0))};
    asio::ip::tcp::endpoint ep{address, port};
    std::cout << boost::format("Server: Starting on: %1%") % ep << std::endl;

    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor{io_context};

    acceptor.open(ep.protocol());
    acceptor.set_option(asio::socket_base::reuse_address(true));
    acceptor.bind(ep);
    acceptor.listen();

    std::vector<std::thread> threads;
    threads.reserve(server_thread_count);
    run_server_and_join(acceptor, server_thread_count, threads);
    return 0;
}

// Runs a UNIX domain sockets server and clients connecting to it.
template <typename Duration>
int run_unix_local_server_clients(std::size_t server_thread_count, std::size_t client_thread_count,
                                  Duration run_duration) {
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
    char const *test_file = "_TMP_local_server_test";
    std::remove(test_file);
    asio::local::stream_protocol::endpoint ep{test_file};
    std::cout << boost::format("Server: Starting on: %1%") % ep << std::endl;

    asio::io_context io_context;
    asio::local::stream_protocol::acceptor acceptor{io_context, ep};

    std::vector<std::thread> threads;
    threads.reserve(1 + client_thread_count + server_thread_count);

    // 1st thread will signal CTRL-C after a while
    threads.emplace_back([&] {
        std::this_thread::sleep_for(run_duration);
        ::raise(SIGINT);
    });

    // The client threads can start as the server acceptor is already set up.
    for (std::size_t c = 0; c < client_thread_count; ++c) {
        threads.emplace_back([c, ep, &io_context] {
            try {
                std::cout << boost::format("Client[%1%]: Started") % c << std::endl;
                asio::local::stream_protocol::socket socket{io_context};
                socket.connect(ep);
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

    run_server_and_join(acceptor, server_thread_count, threads);
    return 0;
#else
    (void)server_thread_count;
    (void)client_thread_count;
    (void)run_duration;
    std::cerr << "Local sockets are not supported" << std::endl;
    return 1;
#endif
}

int main(int argc, char **argv) {
    try {
        using namespace std::literals::chrono_literals;
        return argc > 1 ? run_tcp_server(25, argc, argv) : run_unix_local_server_clients(2, 4, 10s);
    } catch (std::exception const &e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
    }
    return 1;
}
