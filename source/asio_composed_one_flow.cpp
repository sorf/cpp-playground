#include "async_state.hpp"
#include "bind_allocator.hpp"

#include <array>
#include <boost/asio/async_result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/format.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <type_traits>
#include <vector>

namespace asio = boost::asio;

namespace {

using error_code = boost::system::error_code;

// A composed operations that runs many timer wait operations in series.
template <typename CompletionToken>
auto async_one_timer(asio::io_context &io_context, std::chrono::steady_clock::duration run_duration,
                     CompletionToken &&token) ->
    typename asio::async_result<std::decay_t<CompletionToken>, void(error_code)>::return_type {

    struct state_data {
        state_data(asio::io_context &io_context, std::chrono::steady_clock::duration run_duration)
            : internal_timer{io_context}, end_time{std::chrono::steady_clock::now() + run_duration}, waits{} {}

        asio::steady_timer internal_timer;
        std::chrono::steady_clock::time_point const end_time;
        unsigned waits;
    };

    using state_type =
        async_utils::async_state<void(error_code), CompletionToken, asio::io_context::executor_type, state_data>;

    struct internal_op {
        static void start_one_wait(state_type &&state) {
            auto *data = state.get();
            data->internal_timer.expires_after(std::chrono::milliseconds{50});
            data->internal_timer.async_wait(state.wrap()([=, state = std::move(state)](error_code ec) mutable {
                ++data->waits;
                if (!ec && std::chrono::steady_clock::now() < data->end_time) {
                    std::cout << boost::format("internal_op[waits=%d]: After one wait") % data->waits << std::endl;
                    start_one_wait(std::move(state));
                    return;
                }
                // Operation complete here.
                if (!ec) {
                    std::cout << boost::format("internal_op[waits=%d]: Done") % data->waits << std::endl;
                } else {
                    std::cout << boost::format("internal_op[waits=%d]: Error: %s:%s") % data->waits % ec % ec.message()
                              << std::endl;
                }
                state.invoke(ec);
            }));
        }
    };

    typename state_type::completion_type completion(token);
    state_type state{std::move(completion.completion_handler), io_context.get_executor(), io_context, run_duration};
    internal_op::start_one_wait(std::move(state));
    return completion.result.get();
}

// Manages the memory used for handler based custom allocation.
class handler_memory {
  public:
    handler_memory() : m_storage{}, m_in_use{false} {}
    handler_memory(handler_memory const &) = delete;
    handler_memory(handler_memory &&) noexcept = delete;
    handler_memory &operator=(handler_memory const &) = delete;
    handler_memory &operator=(handler_memory &&) noexcept = delete;
    ~handler_memory() = default;

    void *allocate(std::size_t size) {
        void *ptr = nullptr;
        if (size > 0 && size < sizeof(m_storage[0])) {
            for (std::size_t i = 0; i < m_in_use.size(); ++i) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                if (!m_in_use[i]) {
                    std::cout << boost::format("allocated[%d]: Size: %d") % i % size << std::endl;

                    m_in_use[i] = true; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                    ptr = &m_storage[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                    break;
                }
            }
        }

        if (ptr == nullptr) {
            ptr = ::operator new(size);
        }

        return ptr;
    }

    void deallocate(void *ptr) noexcept {
        for (std::size_t i = 0; i < m_storage.size(); ++i) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
            if (ptr == &m_storage[i]) {
                try {
                    std::cout << boost::format("deallocated[%d]") % i << std::endl;
                } catch (...) {
                }

                m_in_use[i] = false; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                ptr = nullptr;
                break;
            }
        }
        ::operator delete(ptr); // no-op for nullptr
    }

  private:
    // Storage
    std::array<std::aligned_storage_t<1024>, 4> m_storage;
    // Usage flag for each storage slot.
    std::array<bool, 4> m_in_use;
};

// Minimal allocator for the handler based custom allocation.
template <typename T> struct handler_allocator {
    using value_type = T;
    explicit handler_allocator(handler_memory &storage) noexcept : m_storage{storage} {}
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
    template <typename U> handler_allocator(handler_allocator<U> const &other) noexcept : m_storage{other.m_storage} {}

    template <typename U> bool operator==(handler_allocator<U> const &other) const noexcept {
        return &m_storage == other.m_storage;
    }
    template <typename U> bool operator!=(handler_allocator<U> const &other) const noexcept {
        return &m_storage != other.m_storage;
    }

    T *allocate(std::size_t count) const { return static_cast<T *>(m_storage.allocate(count * sizeof(T))); }
    void deallocate(T *const ptr, std::size_t /*unused*/) const noexcept { m_storage.deallocate(ptr); }

  private:
    template <typename> friend struct handler_allocator;
    handler_memory &m_storage;
};

void test_callback(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    handler_memory storage;
    std::cout << "[callback] Timers start" << std::endl;
    async_one_timer(io_context, run_duration,
                    async_utils::bind_allocator(handler_allocator<char>(storage), [](error_code const &error) {
                        if (error) {
                            std::cout << "[callback] Timers error: " << error.message() << std::endl;
                        } else {
                            std::cout << "[callback] Timers done" << std::endl;
                        }
                    }));

    io_context.run();
}

void test_callback_strand(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    handler_memory storage;
    auto io_work = asio::make_work_guard(io_context.get_executor());
    std::vector<std::thread> threads;
    int thread_count = 25;
    threads.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&io_context] { io_context.run(); });
    }

    std::cout << "[callback_strand] Timers start" << std::endl;
    asio::strand<asio::io_context::executor_type> strand_timers(io_context.get_executor());

    async_one_timer(
        io_context, run_duration,
        asio::bind_executor(strand_timers,
                            async_utils::bind_allocator(handler_allocator<char>(storage), [](error_code const &error) {
                                if (error) {
                                    std::cout << "[callback_strand] Timers error: " << error.message() << std::endl;
                                } else {
                                    std::cout << "[callback_strand] Timers done" << std::endl;
                                }
                            })));

    io_work.reset();
    io_context.run();
    for (auto &t : threads) {
        t.join();
    }
}

void test_future(std::chrono::steady_clock::duration run_duration) {
    asio::io_context io_context;
    std::cout << "[future] Timers start" << std::endl;

    std::future<void> f = async_one_timer(io_context, run_duration, asio::use_future);
    std::thread thread_wait_future{[f = std::move(f)]() mutable {
        try {
            f.get();
            std::cout << "[future] Timers done" << std::endl;
        } catch (std::exception const &e) {
            std::cout << "[future] Timers error: " << e.what() << std::endl;
        }
    }};
    io_context.run();
    thread_wait_future.join();
}

} // namespace

int main() {
    try {
        test_callback(std::chrono::seconds(1));
        test_callback_strand(std::chrono::seconds(1));
        test_future(std::chrono::seconds(1));
    } catch (std::exception const &e) {
        std::cout << "Error: " << e.what() << "\n";
    }
}
