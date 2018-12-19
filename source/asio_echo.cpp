#include <boost/asio/async_result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core/handler_ptr.hpp>
#include <boost/beast/core/type_traits.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;

namespace {

using error_code = boost::system::error_code;

// A composed operations that reads a message from a socket stream and writes it back.
// It is based on:
// https://github.com/chriskohlhoff/asio/blob/master/asio/src/examples/cpp11/operations/composed_5.cpp
// https://github.com/boostorg/beast/blob/develop/example/echo-op/echo_op.cpp
template <typename StreamSocket, typename CompletionToken>
auto async_echo_rw(StreamSocket &socket, CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code, std::size_t)>::return_type {

    using completion_handler_sig = void(error_code, std::size_t);
    using completion_type = asio::async_completion<CompletionToken, completion_handler_sig>;
    using completion_handler_type = typename completion_type::completion_handler_type;

    template <typename Handler> struct internal_state {
        explicit state(Handler const &handler) : handler{handler} {}
        state(state const &) = delete;
        state(state &&) = delete;
        state &operator=(state const &) = delete;
        state &operator=(state &&) = delete;

        Handler const &handler;
    };

    template <typename Handler> struct async_state {
        template <typename DeducedHandler>
        explicit async_state(DeducedHandler &&handler) : state(std::forward<DeducedHandler>(handle)) {}
        async_state(async_state const &) = delete;
        async_state(async_state &&) = default;
        async_state &operator=(async_state const &) = delete;
        async_state &operator=(async_state &&) = delete;

        beast::handler_ptr<internal_state, Handler> state;
    };

    static_assert(beast::is_async_stream<StreamSocket>::value, "AsyncStream requirements not met");

    completion_type completion(token);
    // std::make_shared<internal_state>(io_context, user_resource, std::move(completion.completion_handler))
    //    ->start_many_waits(run_duration);
    return completion.result.get();
}

} // namespace

int main(void) { return 0; }
