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
template <typename T> void print(T const &v) {
    std::lock_guard<std::mutex> lock{g_print_mutex};
    std::cout << v << std::endl;
}

class thread_names {
  public:
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

template <std::size_t id> struct composed_one_flow {
  private:
    template <typename Handler> static decltype(auto) get_executor(asio::io_context &io_context, Handler &h) {
        return asio::get_associated_executor(h, io_context.get_executor());
    }

  public:
    template <typename Handler> static void start(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto o = operation("composed_one_flow", "start", id);
        asio::post(io_context,
                   asio::bind_executor(get_executor(io_context, h), [&, h = std::forward<Handler>(h)]() mutable {
                       cont1(io_context, std::move(h));
                   }));
    }
    template <typename Handler> static void cont1(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto o = operation("composed_one_flow", "cont1", id);
        asio::post(io_context,
                   asio::bind_executor(get_executor(io_context, h), [&, h = std::forward<Handler>(h)]() mutable {
                       cont2(io_context, std::move(h));
                   }));
    }
    template <typename Handler> static void cont2(asio::io_context &io_context, Handler &&h) {
        [[maybe_unused]] auto o = operation("composed_one_flow", "cont2", id);
        asio::post(io_context, asio::bind_executor(get_executor(io_context, h),
                                                   [&, h = std::forward<Handler>(h)]() mutable { end(std::move(h)); }));
    }
    template <typename Handler> static void end(Handler &&h) {
        [[maybe_unused]] auto o = operation("composed_one_flow", "end", id);
        h();
    }
};

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
    print(boost::format("<< Test: %s\n") % test_name);
}

int main() {
    using strand_type = asio::strand<asio::io_context::executor_type>;
    try {

        runt_test("no-strand\n\t"
                  "operation 100, done-handler 1105\n\t"
                  "operations 200, done-handler 1205",
                  [](asio::io_context &io_context) {
                      composed_one_flow<100>::start(
                          io_context, [] { [[maybe_unused]] auto o = operation("run_test", "done", 1105); });
                      composed_one_flow<200>::start(
                          io_context, [] { [[maybe_unused]] auto o = operation("run_test", "done", 1205); });
                  });

        runt_test("all-in-one-strand except initiation (likely a bug)\n\t"
                  "operation 100, done-handler 1105\n\t"
                  "operations 200, done-handler 1205",
                  [](asio::io_context &io_context) {
                      strand_type strand{io_context.get_executor()};
                      composed_one_flow<100>::start(io_context, asio::bind_executor(strand, [strand] {
                                                        [[maybe_unused]] auto o = operation("run_test", "done", 1105);
                                                    }));
                      composed_one_flow<200>::start(io_context, asio::bind_executor(strand, [strand] {
                                                        [[maybe_unused]] auto o = operation("run_test", "done", 1205);
                                                    }));
                  });

        runt_test("all-in-one-strand\n\t"
                  "operation 100, done-handler 1105\n\t"
                  "operations 200, done-handler 1205",
                  [](asio::io_context &io_context) {
                      strand_type strand{io_context.get_executor()};
                      asio::dispatch(asio::bind_executor(strand, [&, strand] {
                          composed_one_flow<100>::start(io_context, asio::bind_executor(strand, [strand] {
                                                            [[maybe_unused]] auto o =
                                                                operation("run_test", "done", 1105);
                                                        }));
                          composed_one_flow<200>::start(io_context, asio::bind_executor(strand, [strand] {
                                                            [[maybe_unused]] auto o =
                                                                operation("run_test", "done", 1205);
                                                        }));
                      }));
                  });

        runt_test(
            "2-strands\n\t"
            "strand1: operations *00, done-handler 1*05\n\t"
            "strand2: operations *09, done-handler 1*06",
            [](asio::io_context &io_context) {
                {
                    strand_type strand{io_context.get_executor()};
                    asio::dispatch(asio::bind_executor(strand, [&, strand] {
                        composed_one_flow<100>::start(io_context, asio::bind_executor(strand, [strand] {
                                                          [[maybe_unused]] auto o = operation("run_test", "done", 1105);
                                                      }));
                        composed_one_flow<200>::start(io_context, asio::bind_executor(strand, [strand] {
                                                          [[maybe_unused]] auto o = operation("run_test", "done", 1205);
                                                      }));
                    }));
                }
                {
                    strand_type strand{io_context.get_executor()};
                    asio::dispatch(asio::bind_executor(strand, [&, strand] {
                        composed_one_flow<109>::start(io_context, asio::bind_executor(strand, [strand] {
                                                          [[maybe_unused]] auto o = operation("run_test", "done", 1106);
                                                      }));
                        composed_one_flow<209>::start(io_context, asio::bind_executor(strand, [strand] {
                                                          [[maybe_unused]] auto o = operation("run_test", "done", 1206);
                                                      }));
                    }));
                }
            });

        runt_test("2-strands and dispatch in a 3rd\n\t"
                  "strand1: operations *00, done-handler 1*05\n\t"
                  "strand2: operations *09, done-handler 1*06\n\t"
                  "print_strand: executes the done_handlers",
                  [](asio::io_context &io_context) {
                      strand_type print_strand(io_context.get_executor());
                      {
                          strand_type strand{io_context.get_executor()};
                          asio::dispatch(asio::bind_executor(strand, [&, strand, print_strand] {
                              composed_one_flow<100>::start(
                                  io_context, asio::bind_executor(strand, [strand, print_strand] {
                                      asio::dispatch(asio::bind_executor(print_strand, [print_strand] {
                                          [[maybe_unused]] auto o = operation("run_test", "done", 1105);
                                      }));
                                  }));
                              composed_one_flow<200>::start(
                                  io_context, asio::bind_executor(strand, [strand, print_strand] {
                                      asio::dispatch(asio::bind_executor(print_strand, [print_strand] {
                                          [[maybe_unused]] auto o = operation("run_test", "done", 1205);
                                      }));
                                  }));
                          }));
                      }
                      {
                          strand_type strand{io_context.get_executor()};
                          asio::dispatch(asio::bind_executor(strand, [&, strand, print_strand] {
                              composed_one_flow<109>::start(
                                  io_context, asio::bind_executor(strand, [strand, print_strand] {
                                      asio::dispatch(asio::bind_executor(print_strand, [print_strand] {
                                          [[maybe_unused]] auto o = operation("run_test", "done", 1106);
                                      }));
                                  }));
                              composed_one_flow<209>::start(
                                  io_context, asio::bind_executor(strand, [strand, print_strand] {
                                      asio::dispatch(asio::bind_executor(print_strand, [print_strand] {
                                          [[maybe_unused]] auto o = operation("run_test", "done", 1206);
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
