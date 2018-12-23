#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/handler_alloc_hook.hpp>
#include <boost/asio/handler_continuation_hook.hpp>
#include <boost/asio/handler_invoke_hook.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/type_traits.hpp>
#include <boost/predef.h>
#include <boost/scope_exit.hpp>
#include <iostream>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;

//----------------------------------------------------------------------------------------------------------------------
// bind_allocator

namespace {

template <typename T, typename Allocator> struct allocator_binder {
    allocator_binder(T &&target, Allocator &&allocator) : target(std::move(target)), allocator(std::move(allocator)) {}

    using target_type = T;
    using allocator_type = Allocator;

    target_type &get() noexcept { return this->target; }
    const target_type &get() const { return this->target; }
    allocator_type get_allocator() const noexcept { return this->allocator; }

    template <class... Args> decltype(auto) operator()(Args &&... args) const {
        std::invoke(target, std::forward<Args>(args)...);
    }

    template <class... Args> decltype(auto) operator()(Args &&... args) {
        std::invoke(target, std::forward<Args>(args)...);
    }

    target_type target;
    allocator_type allocator;
};

// Associates an object of type T with an allocator.
// Similar to boost::asio::bind_executor
template <typename Allocator, typename T> inline decltype(auto) bind_allocator(Allocator &&allocator, T &&target) {
    return allocator_binder<typename std::decay_t<T>, Allocator>(std::forward<T>(target),
                                                                 std::forward<Allocator>(allocator));
}

} // namespace

namespace boost {
namespace asio {

template <typename T, typename Allocator, typename Allocator1>
struct associated_allocator<allocator_binder<T, Allocator>, Allocator1> {
    using type = Allocator;
    static type get(const allocator_binder<T, Allocator> &b, const Allocator1 & /*unused*/ = Allocator1()) noexcept {
        return b.get_allocator();
    }
};

template <typename T, typename Allocator, typename Executor1>
struct associated_executor<allocator_binder<T, Allocator>, Executor1> {
    using type = typename associated_executor<T, Executor1>::type;
    static type get(const allocator_binder<T, Allocator> &b, const Executor1 &e = Executor1()) noexcept {
        return associated_executor<T, Executor1>::get(b.get(), e);
    }
};

} // namespace asio
} // namespace boost

//----------------------------------------------------------------------------------------------------------------------
// async_state

namespace {

template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor, typename StateData>
class async_state {

    struct state_holder;

  public:
    using completion_type = asio::async_completion<CompletionToken, CompletionHandlerSignature>;
    using completion_handler_type = typename completion_type::completion_handler_type;
    using default_allocator = std::allocator<void>;
    using completion_handler_allocator_type = asio::associated_allocator_t<completion_handler_type, default_allocator>;

    template <class... Args>
    explicit async_state(completion_handler_type &&completion_handler, Executor &&executor, Args &&... args)
        : m_state{async_state::make_state(std::move(completion_handler), std::move(executor),
                                          std::forward<Args>(args)...)} {}

    async_state(async_state const &) = delete;
    async_state(async_state &&) noexcept = default;
    async_state &operator=(async_state const &) = delete;
    async_state &operator=(async_state &&) noexcept = default;
    ~async_state() = default;

    StateData *get() const {
        assert(m_state);
        return m_state.get();
    }

    StateData &operator*() const {
        assert(m_state);
        return *m_state;
    }

    StateData *operator->() const {
        assert(m_state);
        return m_state;
    }

    class wrapper {
      public:
        explicit wrapper(state_holder *state) : m_state{state} {}
        template <typename F> decltype(auto) operator()(F &&f) const {
            // The final completion handler should not have been called yet (and this has not been moved).
            assert(m_state);
            assert(m_state->io_work.owns_work());
            auto allocator = asio::get_associated_allocator(m_state->completion_handler, default_allocator{});
            auto executor = asio::get_associated_executor(m_state->completion_handler, m_state->executor);
            return asio::bind_executor(std::move(executor), bind_allocator(std::move(allocator), std::forward<F>(f)));
        }

      private:
        state_holder *m_state;
    };

    wrapper wrap() { return wrapper{m_state.get()}; }

    template <class... Args> void invoke(Args &&... args) {
        // The final completion handler should be called at most once.
        assert(m_state);
        auto completion_handler = std::move(m_state->completion_handler);
        m_state.reset();
        std::invoke(completion_handler, std::forward<Args>(args)...);
    }

  private:
    struct state_holder_base {
        state_holder_base(completion_handler_type &&completion_handler, Executor &&executor_arg)
            : completion_handler(std::move(completion_handler)),
              executor(std::move(executor_arg)), io_work{asio::make_work_guard(executor)} {}

        completion_handler_type completion_handler;
        Executor executor;
        asio::executor_work_guard<Executor> io_work;
    };

    struct state_holder : public state_holder_base, public StateData {
        template <class... Args>
        state_holder(completion_handler_type &&completion_handler, Executor &&executor, Args &&... args)
            : state_holder_base(std::move(completion_handler), std::move(executor)),
              StateData(std::forward<Args>(args)...) {}
    };

    using state_allocator_type =
        typename std::allocator_traits<completion_handler_allocator_type>::template rebind_alloc<state_holder>;
    using state_allocator_traits = std::allocator_traits<state_allocator_type>;

    struct state_deleter {
        explicit state_deleter(state_allocator_type const &a) : m_allocator(a) {}
        using pointer = typename state_allocator_traits::pointer;
        void operator()(pointer p) const {
            state_allocator_type a(m_allocator);
            state_allocator_traits::destroy(a, std::addressof(*p));
            state_allocator_traits::deallocate(a, p, 1);
        }

      private:
        state_allocator_type m_allocator;
    };
    using state_holder_ptr = std::unique_ptr<state_holder, state_deleter>;

    template <class... Args>
    static state_holder_ptr make_state(completion_handler_type &&completion_handler, Executor &&executor,
                                       Args &&... args);

    state_holder_ptr m_state;
};

template <typename CompletionHandlerSignature, typename CompletionToken, typename Executor, typename StateData>
template <class... Args>
auto async_state<CompletionHandlerSignature, CompletionToken, Executor, StateData>::make_state(
    completion_handler_type &&completion_handler, Executor &&executor, Args &&... args) -> state_holder_ptr {

    static_assert(!std::is_array<StateData>::value);
    state_allocator_type state_allocator{asio::get_associated_allocator(completion_handler, default_allocator{})};
    auto p = state_allocator_traits::allocate(state_allocator, 1);
    bool commit = false;
    BOOST_SCOPE_EXIT_ALL(&) {
        if (!commit) {
            state_allocator_traits::deallocate(state_allocator, p, 1);
            p = nullptr;
        }
    };
    state_allocator_traits::construct(state_allocator, std::addressof(*p), std::move(completion_handler),
                                      std::move(executor), std::forward<Args>(args)...);
    BOOST_SCOPE_EXIT_ALL(&) {
        if (!commit) {
            state_allocator_traits::destroy(state_allocator, std::addressof(*p));
        }
    };

    state_holder_ptr state{p, state_deleter(state_allocator)};
    commit = true;
    return state;
}

//----------------------------------------------------------------------------------------------------------------------
// async_echo_rw

using error_code = boost::system::error_code;

// A composed operations that reads a message from a socket stream and writes it back. It is based on:
// https://github.com/chriskohlhoff/asio/blob/master/asio/src/examples/cpp11/operations/composed_5.cpp
// https://github.com/boostorg/beast/blob/develop/example/echo-op/echo_op.cpp
// but attempts an implementation that uses lambda expressions for the internal completion handlers.
template <typename StreamSocket, typename CompletionToken>
auto async_echo_rw(StreamSocket &socket, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    struct state_data {
        state_data(StreamSocket &socket, std::size_t buffer_size) : socket{socket}, echo_buffer(buffer_size, '\0') {}
        StreamSocket &socket;
        std::vector<char> echo_buffer;
    };

    using state_type =
        async_state<void(error_code, std::size_t), CompletionToken, typename StreamSocket::executor_type, state_data>;

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
