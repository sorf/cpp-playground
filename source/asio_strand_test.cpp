// Runs different scenarios with Boost.Asio strands in a thread pool.
//
// The prints to std output try to depict how various operations execute concurrently or serially based on
// how they are distributed between strands.
// Each operation consists of:
//     print: '\' enter [<operation-id>]<operation name>
//     sleep <operation-id> milliseconds
//     (optionally) schedule the next operation
//     sleep <operation-id> milliseconds
//     print: '/' exit [<operation-id>]<operation name>
//
// The way successive '\' and '/' are printed (their position in the message is operation-id % 10)
// should make more easy to visualize operations that
// - executed concurrently:
//      \      or    \         or      \
//      \                \                   \
//      /            /                       /
//      /                /             /
// - executed serially:
//      \         or   \
//      /              /
//      \                  \
//      /                  /
//
// The operation-ids used below are chosen based on the duration of the sleep they will generate
// and the position of the '\', '/' markers that will be printed for them.
// For example: operation-ids 100 and 200 will be considered "related" (e.g. they shouldn't
// execute concurrently) as they will be printed at column 0, while operation-ids 109 and 209
// will represent a different group of "related" operations that will have their markers printed at
// column 9.

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/format.hpp>
#include <iostream>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace asio = boost::asio;

std::mutex g_print_mutex;
// Prints to std::cout under a mutex
template <typename T> void print(T const &v) {
    std::lock_guard<std::mutex> lock{g_print_mutex};
    std::cout << v << std::endl;
}

// Human readable thread-ids.
class thread_names {
  public:
    // Returns the name of the current thread.
    static std::string const &this_thread() {
        auto &names = instance();
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> lock{names.m_mutex};
        auto i = names.m_names.find(tid);
        if (i == names.m_names.end()) {
            i = names.m_names.try_emplace(tid, boost::str(boost::format("thread-%d") % names.m_next_id++)).first;
        }
        return i->second;
    }
    // Clears the name map.
    static void clear() {
        auto &names = instance();
        std::lock_guard<std::mutex> lock{names.m_mutex};
        names.m_next_id = 0;
        names.m_names.clear();
    }

  private:
    static thread_names &instance() {
        static thread_names names;
        return names;
    }

    std::map<std::thread::id, std::string> m_names;
    std::size_t m_next_id = 0;
    std::mutex m_mutex;
};

// Basic scope-exit to help automate the actions executed when an operation ends.
template <typename F> struct [[nodiscard]] scope_exit {
    explicit scope_exit(F && f) : f(std::move(f)) {}
    scope_exit(scope_exit const &) = delete;
    scope_exit(scope_exit &&) = delete;
    scope_exit &operator=(scope_exit const &) = delete;
    scope_exit &operator=(scope_exit &&other) = delete;
    ~scope_exit() { f(); }
    F f;
};
template <typename F> decltype(auto) on_scope_exit(F &&f) { return scope_exit<F>(std::forward<F>(f)); }

// The implementation of all the operations in this example.
// It executes:
// - prints enter operation
// - sleeps
// - returns a scope_exit that:
//      - sleeps
//      - prints exit operation
decltype(auto) operation(std::string_view classname, std::string_view method, std::size_t id) {
    std::string pre_marker(id % 10, ' ');
    std::string post_marker(10 - id % 10, ' ');
    print(boost::format("[%s] %s\\%s enter [%d]%s::%s") % thread_names::this_thread() % pre_marker % post_marker % id %
          classname % method);
    std::this_thread::sleep_for(std::chrono::milliseconds{id});
    return on_scope_exit([=] {
        std::this_thread::sleep_for(std::chrono::milliseconds{id});
        print(boost::format("[%s] %s/%s  exit [%d]%s::%s") % thread_names::this_thread() % pre_marker % post_marker %
              id % classname % method);
    });
}

// A very basic asynchronous composed operation implemented in terms of the operation() above.
// The chain of asynchronous operations it executes: 
// - start: asio::post(cont1)
// - cont1: asio::post(cont2)
// - cont2: asio::post(end)
// - end: calls the final completion handler
template <std::size_t id> struct composed_operation {
  private:
    template <typename Handler> static decltype(auto) get_executor(asio::io_context &io_context, Handler &h) {
        return asio::get_associated_executor(h, io_context.get_executor());
    }

  public:
    template <typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto o = operation("composed_operation", "start", id);
        asio::post(io_context,
                   asio::bind_executor(get_executor(io_context, h), [&, h = std::forward<Handler>(h)]() mutable {
                       cont1(io_context, std::move(h));
                   }));
    }
    template <typename Handler> static void cont1(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto o = operation("composed_operation", "cont1", id);
        asio::post(io_context,
                   asio::bind_executor(get_executor(io_context, h), [&, h = std::forward<Handler>(h)]() mutable {
                       cont2(io_context, std::move(h));
                   }));
    }
    template <typename Handler> static void cont2(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto o = operation("composed_operation", "cont2", id);
        asio::post(io_context, asio::bind_executor(get_executor(io_context, h),
                                                   [&, h = std::forward<Handler>(h)]() mutable { end(std::move(h)); }));
    }
    template <typename Handler> static void end(Handler &&h) {
        [[maybe_unused]] auto o = operation("composed_operation", "end", id);
        h();
    }
};


// Sets up an io_context to be run by a thread-pool and calls a test function with it.
template <typename F> void runt_test(std::string_view test_name, F &&f) {
    thread_names::clear();

    print(boost::format("\n>> Test: %s") % test_name);
    asio::io_context io_context;
    auto threads_io_work = asio::make_work_guard(io_context.get_executor());
    std::vector<std::thread> threads;
    unsigned thread_count = 5;
    threads.reserve(thread_count);
    for (unsigned i = 0; i < thread_count; ++i) {
        threads.emplace_back([&io_context] { io_context.run(); });
    }
    f(io_context);
    threads_io_work.reset();
    io_context.run();
    for (auto &t : threads) {
        t.join();
    }
    print("<< Test\n");
}

int main() {
    using strand_type = asio::strand<asio::io_context::executor_type>;
    try {
        // Two composed operations without any strand.
        // Their internal completion handlers will run concurrently; even concurrently with the initiation function
        // (e.g. start() running in parallel with cont1() for the same operation id) 
        runt_test("no strand\n\t"
                  "composed_operation 100 with final-handler 1105\n\t"
                  "composed_operation 200 with final-handler 1205",
                  [](asio::io_context &io_context) {
                      composed_operation<100>::start(
                          io_context, [] { [[maybe_unused]] auto o = operation("", "final-handler", 1105); });
                      composed_operation<200>::start(
                          io_context, [] { [[maybe_unused]] auto o = operation("", "final-handler", 1205); });
                  });

        // The first attempt to run both operations so that their handlers
        // (both internal and the final completion) do not execute concurrently
        runt_test("all in one strand except initiation (likely a bug)\n\t"
                  "composed_operation 100 with final-handler 1105\n\t"
                  "composed_operation 200 with final-handler 1205",
                  [](asio::io_context &io_context) {
                      strand_type strand{io_context.get_executor()};
                      composed_operation<100>::start(io_context, asio::bind_executor(strand, [strand] {
                                                        [[maybe_unused]] auto o = operation("", "final-handler", 1105);
                                                    }));
                      composed_operation<200>::start(io_context, asio::bind_executor(strand, [strand] {
                                                        [[maybe_unused]] auto o = operation("", "final-handler", 1205);
                                                    }));
                  });

        // No concurrent execution of initiation and completion handler functions.
        runt_test("all in one strand\n\t"
                  "composed_operation 100 with final-handler 1105\n\t"
                  "composed_operation 200 with final-handler 1205",
                  [](asio::io_context &io_context) {
                      strand_type strand{io_context.get_executor()};
                      asio::dispatch(asio::bind_executor(strand, [&, strand] {
                          composed_operation<100>::start(io_context, asio::bind_executor(strand, [strand] {
                                                            [[maybe_unused]] auto o =
                                                                operation("", "final-handler", 1105);
                                                        }));
                          composed_operation<200>::start(io_context, asio::bind_executor(strand, [strand] {
                                                            [[maybe_unused]] auto o =
                                                                operation("", "final-handler", 1205);
                                                        }));
                      }));
                  });

        // Two pairs of composed operations running in their own strand, including their initiation functions.
        // The final completion handlers are called from these two strands
        // (so some execute in a strand, some in the other).
        runt_test(
            "2-strands\n\t"
            "strand1: composed_operations *00 with final-handlers 1*05\n\t"
            "strand2: composed_operations *09 with final-handlers 1*06",
            [](asio::io_context &io_context) {
                {
                    strand_type strand{io_context.get_executor()};
                    asio::dispatch(asio::bind_executor(strand, [&, strand] {
                        composed_operation<100>::start(io_context, asio::bind_executor(strand, [strand] {
                                                          [[maybe_unused]] auto o = operation("", "final-handler", 1105);
                                                      }));
                        composed_operation<200>::start(io_context, asio::bind_executor(strand, [strand] {
                                                          [[maybe_unused]] auto o = operation("", "final-handler", 1205);
                                                      }));
                    }));
                }
                {
                    strand_type strand{io_context.get_executor()};
                    asio::dispatch(asio::bind_executor(strand, [&, strand] {
                        composed_operation<109>::start(io_context, asio::bind_executor(strand, [strand] {
                                                          [[maybe_unused]] auto o = operation("", "final-handler", 1106);
                                                      }));
                        composed_operation<209>::start(io_context, asio::bind_executor(strand, [strand] {
                                                          [[maybe_unused]] auto o = operation("", "final-handler", 1206);
                                                      }));
                    }));
                }
            });

        // Two pairs of composed operations running in their own strand, the final completion handlers are run in a 3rd strand.
        runt_test("2-strands and dispatch in a 3rd\n\t"
                  "strand1: composed_operations *00 with final-handlers 1*05\n\t"
                  "strand2: composed_operations *09 with final-handlers 1*06\n\t"
                  "print_strand: executes the final-handlers",
                  [](asio::io_context &io_context) {
                      strand_type print_strand(io_context.get_executor());
                      {
                          strand_type strand{io_context.get_executor()};
                          asio::dispatch(asio::bind_executor(strand, [&, strand, print_strand] {
                              composed_operation<100>::start(
                                  io_context, asio::bind_executor(strand, [strand, print_strand] {
                                      asio::dispatch(asio::bind_executor(print_strand, [print_strand] {
                                          [[maybe_unused]] auto o = operation("", "final-handler", 1105);
                                      }));
                                  }));
                              composed_operation<200>::start(
                                  io_context, asio::bind_executor(strand, [strand, print_strand] {
                                      asio::dispatch(asio::bind_executor(print_strand, [print_strand] {
                                          [[maybe_unused]] auto o = operation("", "final-handler", 1205);
                                      }));
                                  }));
                          }));
                      }
                      {
                          strand_type strand{io_context.get_executor()};
                          asio::dispatch(asio::bind_executor(strand, [&, strand, print_strand] {
                              composed_operation<109>::start(
                                  io_context, asio::bind_executor(strand, [strand, print_strand] {
                                      asio::dispatch(asio::bind_executor(print_strand, [print_strand] {
                                          [[maybe_unused]] auto o = operation("", "final-handler", 1106);
                                      }));
                                  }));
                              composed_operation<209>::start(
                                  io_context, asio::bind_executor(strand, [strand, print_strand] {
                                      asio::dispatch(asio::bind_executor(print_strand, [print_strand] {
                                          [[maybe_unused]] auto o = operation("", "final-handler", 1206);
                                      }));
                                  }));
                          }));
                      }
                  });

    } catch (std::exception const &e) {
        std::cout << "Error: " << e.what() << "\n";
    }
    return 0;
}
