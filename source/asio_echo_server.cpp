//#define ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG
//#define BOOST_ASIO_ENABLE_HANDLER_TRACKING

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
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

namespace asio = boost::asio;
using error_code = boost::system::error_code;

namespace async_utils {

// Helper base class to add a 'continue_handler' method in derived classes.
// It expects:
//      - bool Derived::is_open()
//      - Derived::close()
//      - Derived::try_invoke()
template <class Derived> struct enable_continue_handler {

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

// Helper base class to add a 'continue_handler' method in derived classes and support for verifying that there are no
// concurrent completion handlers.
// It expects:
//      - bool Derived::is_open()
//      - Derived::close()
//      - Derived::try_invoke()
//      - std::atomic_bool &Derived::executing()
template <class Derived> struct enable_continue_handler_not_concurrent {
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
        std::atomic_bool &executing = pthis->executing();
        BOOST_ASSERT(!executing);
        executing = true;
        return make_check_scope_exit([&executing] {
            BOOST_ASSERT(executing);
            executing = false;
        });
    }

    // Same as enable_continue_handler::continue_handler() but it disables a check scope guard before invoking the
    // final handler.
    template <typename CheckScopeExit = std::optional<unsigned>>
    bool continue_handler(error_code ec, CheckScopeExit *check_scope_exit = nullptr) {
        auto *pthis = static_cast<Derived *>(this);
        if (pthis->is_open() && !ec) {
            return true;
        }

        if (pthis->is_open()) {
            pthis->close(ec);
        }

        if (check_scope_exit != nullptr) {
            check_scope_exit->reset();
        }

        pthis->try_invoke();
        return false;
    }

    template <typename CheckScopeExit> bool continue_handler(error_code ec, CheckScopeExit &check_scope_exit) {
        return continue_handler(ec, &check_scope_exit);
    }
};

} // namespace async_utils

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
              write_timer{close_timer.get_executor().context()}, executing{}, is_open{}, total_write_size{} {}

        StreamSocket &socket;
        asio::steady_timer &close_timer;
        std::size_t const max_size = 128;
        buffer_t reading_buffer;
        buffer_t read_buffer;
        buffer_t writing_buffer;
        asio::steady_timer write_timer;
        std::atomic_bool executing;
        bool is_open;
        error_code user_completion_error;
        std::size_t total_write_size;
    };

    using base_type =
        async_utils::shared_async_state<signature, CompletionToken, asio::io_context::executor_type, state_data>;
    struct internal_op : base_type, async_utils::enable_continue_handler_not_concurrent<internal_op> {
        using base_type::wrap; // MSVC workaround ('this->` fails to compile in lambdas that copy 'this')
        using async_utils::enable_continue_handler_not_concurrent<internal_op>::check_not_concurrent;
        using async_utils::enable_continue_handler_not_concurrent<internal_op>::continue_handler;
        state_data &data;

        internal_op(StreamSocket &socket, asio::steady_timer &close_timer, buffer_allocator const &allocator,
                    typename base_type::completion_handler_type &&completion_handler)
            : base_type{socket.get_executor(), std::move(completion_handler), socket, close_timer, allocator},
              data{base_type::get_data()} {

            // Waiting for the signal to close this (with either the async_wait error or operation_aborted)
            data.close_timer.async_wait(wrap([*this](error_code ec) mutable {
                auto check = check_not_concurrent();
                if (continue_handler(ec, check)) {
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
            data.write_timer.expires_after(std::chrono::seconds(1));
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

        std::atomic_bool &executing() { return data.executing; }
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
        void try_invoke() {
            assert(!executing()); // This ensures there is no outstanding `check_not_concurrent` scope guard.
            this->try_invoke_move_args(data.user_completion_error, data.total_write_size);
        }
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
            : acceptor{acceptor}, close_timer{close_timer}, is_open{}, total_client_count{} {}

        Acceptor &acceptor;
        asio::steady_timer &close_timer;
        std::map<endpoint_type, std::tuple<socket_type, asio::steady_timer>> clients;
        bool is_open;
        error_code user_completion_error;
        std::size_t total_client_count;
    };

    using base_type =
        async_utils::shared_async_state<signature, CompletionToken, asio::io_context::executor_type, state_data>;
    struct internal_op : base_type, async_utils::enable_continue_handler<internal_op> {
        using base_type::wrap;
        using async_utils::enable_continue_handler<internal_op>::continue_handler;
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
            std::cout << boost::format("client[%1%]: Connected") % i->first << std::endl;
            async_repeat_echo(
                std::get<0>(i->second), std::get<1>(i->second), [*this, i](error_code ec, std::size_t bytes) mutable {
                    std::cout << boost::format("client[%1%]: Disconnected: transferred: %2% (closing error: %3%:%4%)") %
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

using server_close_status = std::pair<error_code, std::size_t>;

// Runs the echo server until CTRL-C.
// The completing handler will receive a 'server_close_status'.
template <typename Acceptor, typename CompletionToken>
auto async_echo_server_until_ctrl_c(Acceptor &acceptor, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(server_close_status)>::return_type {

    using signature = void(server_close_status);
    struct state_data {
        explicit state_data(Acceptor &acceptor)
            : acceptor{acceptor}, signals{acceptor.get_executor().context(), SIGINT},
              close_timer{acceptor.get_executor().context()}, is_open{}, total_client_count{} {}

        Acceptor &acceptor;
        asio::signal_set signals;
        asio::steady_timer close_timer;
        bool is_open;
        error_code user_completion_error;
        std::size_t total_client_count;
    };

    using base_type =
        async_utils::shared_async_state<signature, CompletionToken, asio::io_context::executor_type, state_data>;
    struct internal_op : base_type, async_utils::enable_continue_handler<internal_op> {
        using base_type::wrap;
        using async_utils::enable_continue_handler<internal_op>::continue_handler;
        state_data &data;

        internal_op(Acceptor &acceptor, typename base_type::completion_handler_type &&completion_handler)
            : base_type{acceptor.get_executor(), std::move(completion_handler), acceptor}, data{base_type::get_data()} {

            data.close_timer.expires_at(asio::steady_timer::time_point::max());
            data.signals.async_wait(wrap([*this](error_code ec, int /*unused*/) mutable {
                if (ec) {
                    std::cout << boost::format("Error waiting for signal: %1%:%2%") % ec % ec.message() << std::endl;
                } else {
                    std::cout << "\nCTRL-C detected" << std::endl;
                }
                if (continue_handler(ec)) {
                    close(asio::error::operation_aborted);
                }
            }));

            async_echo_server(data.acceptor, data.close_timer,
                              wrap([*this](error_code ec, std::size_t client_count) mutable {
                                  data.user_completion_error = ec;
                                  data.total_client_count = client_count;
                                  try_invoke();
                              }));

            data.is_open = true;
        }

        bool is_open() const { return data.is_open; }
        void close(error_code ec) {
            data.user_completion_error = ec;
            data.is_open = false;
            data.close_timer.cancel();
            error_code ignored;
            data.signals.cancel(ignored);
        }
        void try_invoke() {
            this->try_invoke_move_args(std::make_pair(data.user_completion_error, data.total_client_count));
        }
    };

    typename internal_op::completion_type completion(token);
    internal_op op{acceptor, std::move(completion.completion_handler)};
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
        asio::ip::tcp::acceptor acceptor{io_context};
        asio::ip::tcp::endpoint endpoint{address, port};
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();

        // We run the server in a thread-pool and we wait for completion with a future.
        auto threads_io_work = asio::make_work_guard(io_context.get_executor());
        std::vector<std::thread> threads;
        unsigned thread_count = 25;
        threads.reserve(thread_count);
        for (unsigned i = 0; i < thread_count; ++i) {
            threads.emplace_back([&io_context] { io_context.run(); });
        }

        std::cout << boost::format("Server: Starting on: %1%") % endpoint << std::endl;
        std::future<server_close_status> f = async_echo_server_until_ctrl_c(acceptor, asio::use_future);
        try {
            auto status = f.get();
            std::cout << boost::format("Server: Stopped: clients: %2% (closing error: %3%:%4%)") % endpoint %
                             status.second % status.first % status.first.message()
                      << std::endl;
        } catch (std::exception const &e) {
            std::cout << "Server: Run error: " << e.what() << std::endl;
        }

        threads_io_work.reset();
        for (auto &t : threads) {
            t.join();
        }

    } catch (std::exception const &e) {
        std::cerr << arg_program << ": Fatal error: " << e.what() << std::endl;
    }
    return 0;
}