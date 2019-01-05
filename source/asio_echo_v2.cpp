#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
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

template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor> class async_state_base {
    struct state_holder;

  public:
    using completion_type = asio::async_completion<CompletionToken, CompletionHandlerSignature>;
    using completion_handler_type = typename completion_type::completion_handler_type;

    explicit async_state_base(completion_handler_type &&completion_handler, Executor &&executor)
        /*TODO(sorf): Use handler allocator for m_state*/
        : m_state{std::make_unique<state_holder>(std::move(completion_handler), std::move(executor))} {}

    async_state_base(async_state_base const &) = delete;
    async_state_base(async_state_base &&) noexcept = default;
    async_state_base &operator=(async_state_base const &) = delete;
    async_state_base &operator=(async_state_base &&) noexcept = default;
    ~async_state_base() = default;

#if BOOST_COMP_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-local-typedefs"
#endif
    using allocator_type = asio::associated_allocator_t<completion_handler_type, std::allocator<void>>;
    allocator_type get_allocator() const noexcept {
        assert(m_state);
        return asio::get_associated_allocator(m_state->completion_handler, std::allocator<void>{});
    }
#if BOOST_COMP_CLANG
#pragma clang diagnostic pop
#endif

    class wrapper {
      public:
        explicit wrapper(state_holder *state) : m_state{state} {}
        template <typename F> decltype(auto) operator()(F &&f) const {
            // The final completion handler should not have been called yet (and this has not been moved).
            assert(m_state);
            assert(m_state->io_work.owns_work());
            return asio::bind_executor(asio::get_associated_executor(m_state->completion_handler, m_state->executor),
                                       std::forward<F>(f));
        }

      private:
        state_holder *m_state;
    };

    wrapper wrap() { return wrapper{m_state.get()}; }

  protected:
    template <class... Args> void call_handler_base(Args &&... args) {
        // The final completion handler should be called at most once.
        assert(m_state);
        auto completion_handler = std::move(m_state->completion_handler);
        m_state.reset();
        std::invoke(completion_handler, std::forward<Args>(args)...);
    }

  private:
    struct state_holder {
        state_holder(completion_handler_type &&completion_handler, Executor &&executor_arg)
            : completion_handler(std::move(completion_handler)),
              executor(std::move(executor_arg)), io_work{asio::make_work_guard(executor)} {}

        completion_handler_type completion_handler;
        Executor executor;
        asio::executor_work_guard<Executor> io_work;
    };
    std::unique_ptr<state_holder> m_state;
};

using error_code = boost::system::error_code;

// A composed operations that reads a message from a socket stream and writes it back. It is based on:
// https://github.com/chriskohlhoff/asio/blob/master/asio/src/examples/cpp11/operations/composed_5.cpp
// https://github.com/boostorg/beast/blob/develop/example/echo-op/echo_op.cpp
// but attempts an implementation that uses lambda expressions for the internal completion handlers.
template <typename StreamSocket, typename CompletionToken>
auto async_echo_rw(StreamSocket &socket, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using state_base =
        async_state_base<void(error_code, std::size_t), CompletionToken, typename StreamSocket::executor_type>;
    struct internal_state : public state_base {
        explicit internal_state(StreamSocket &socket, typename state_base::completion_handler_type &&completion_handler)
            : state_base(std::move(completion_handler), socket.get_executor()), socket(socket),
              echo_buffer(128 /*TODO(sorf): Use handler allocator*/) {}

        // clang-format off
        static void async_echo(internal_state&& self) {
            auto read_buffer = asio::buffer(self.echo_buffer);
            self.socket.async_read_some(
                read_buffer,
                self.wrap()([self = std::move(self)](error_code ec, std::size_t bytes) mutable {
                        if (!ec) {
                            auto write_buffer = asio::buffer(self.echo_buffer.data(), bytes);
                            asio::async_write(
                                self.socket,
                                write_buffer,
                                self.wrap()([self = std::move(self), bytes](error_code ec, std::size_t) mutable {
                                        self.call_handler(ec, bytes);
                                    }));
                        } else {
                            self.call_handler(ec, bytes);
                        }
                    }));
        }
        // clang-format on

        void call_handler(error_code ec, std::size_t bytes) {
            // TODO(sorf): Deallocate the echo_buffer (especially when using the handler allocator)
            this->call_handler_base(ec, bytes);
        }

        StreamSocket &socket;
        std::vector<char> echo_buffer;
    };

    static_assert(beast::is_async_stream<StreamSocket>::value, "AsyncStream requirements not met");
    typename state_base::completion_type completion(token);
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
    char const *arg_program = argv[0]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    char const *arg_address = argv[1]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    char const *arg_port = argv[2];    // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    try {
        auto const address{asio::ip::make_address(arg_address)};
        auto const port{static_cast<std::uint16_t>(std::strtol(arg_port, nullptr, 0))};

        asio::io_context io_context;
        asio::ip::tcp::acceptor acceptor{io_context};
        asio::ip::tcp::endpoint ep{address, port};
        acceptor.open(ep.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(ep);
        acceptor.listen();
#ifndef __clang_analyzer__
        auto socket = acceptor.accept();
#else
        asio::ip::tcp::socket socket{io_context};
#endif
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
