#ifndef STACK_HANDLER_ALLOCATOR_HPP
#define STACK_HANDLER_ALLOCATOR_HPP

#include <array>
#include <cstdlib>
#include <mutex>
#include <new>

#if defined(ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG)
#include <boost/format.hpp>
#include <iostream>
#endif

namespace async_utils {

class stack_handler_memory_base {
  public:
    virtual void *allocate(std::size_t size) = 0;
    virtual void deallocate(void *ptr) noexcept = 0;
};

// The memory manager used by stack_handler_allocator.
// It maintains a fixed number of storage slots each of a fixed size.
// Once all the slots are used, any additional allocation is delegated to the global heap.
template <std::size_t storage_count = 4, std::size_t storage_size = 1024>
class stack_handler_memory : public stack_handler_memory_base {
  public:
    stack_handler_memory() : m_storage{}, m_in_use{false} {}
    stack_handler_memory(stack_handler_memory const &) = delete;
    stack_handler_memory(stack_handler_memory &&) noexcept = delete;
    stack_handler_memory &operator=(stack_handler_memory const &) = delete;
    stack_handler_memory &operator=(stack_handler_memory &&) noexcept = delete;
    ~stack_handler_memory() = default;

    void *allocate(std::size_t size) final {
        lock_guard lock(m_mutex);
        void *ptr = nullptr;
        if (size > 0 && size < sizeof(m_storage[0])) {
            for (std::size_t i = 0; i < m_in_use.size(); ++i) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                if (!m_in_use[i]) {
#if defined(ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG)
                    std::cout << boost::format("handler: allocated[%d]: size: %d") % i % size << std::endl;
#endif
                    m_in_use[i] = true;  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                    ptr = &m_storage[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                    break;
                }
            }
        }

        if (ptr == nullptr) {
#if defined(ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG)
            std::cout << boost::format("handler: global new: size: %d") % size << std::endl;
#endif
            ptr = ::operator new(size);
        }

        return ptr;
    }

    void deallocate(void *ptr) noexcept final {
        lock_guard lock(m_mutex);
        for (std::size_t i = 0; i < m_storage.size(); ++i) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
            if (ptr == &m_storage[i]) {
#if defined(ASYNC_UTILS_STACK_HANDLER_ALLOCATOR_DEBUG)
                try {
                    std::cout << boost::format("handler: deallocated[%d]") % i << std::endl;
                } catch (...) {
                }
#endif
                m_in_use[i] = false; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
                ptr = nullptr;
                break;
            }
        }
        ::operator delete(ptr); // no-op for nullptr
    }

  private:
    // Storage
    std::array<std::aligned_storage_t<storage_size>, storage_count> m_storage;
    // Usage flag for each storage slot.
    std::array<bool, storage_count> m_in_use;

    using lock_guard = std::lock_guard<std::mutex>;
    std::mutex m_mutex;
};

// A basic allocator that allocates from a fixed number of storage slots each of a fixed size.
// Once all the slots are used, any additional allocation is delegated to the global heap.
template <typename T> struct stack_handler_allocator {
    using value_type = T;
    explicit stack_handler_allocator(stack_handler_memory_base &storage) noexcept : m_storage{storage} {}
    template <typename U> // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
    stack_handler_allocator(stack_handler_allocator<U> const &other) noexcept : m_storage{other.m_storage} {}

    template <typename U> bool operator==(stack_handler_allocator<U> const &other) const noexcept {
        return &m_storage == other.m_storage;
    }
    template <typename U> bool operator!=(stack_handler_allocator<U> const &other) const noexcept {
        return &m_storage != other.m_storage;
    }

    T *allocate(std::size_t count) const { return static_cast<T *>(m_storage.allocate(count * sizeof(T))); }
    void deallocate(T *const ptr, std::size_t /*unused*/) const noexcept { m_storage.deallocate(ptr); }

  private:
    template <typename> friend struct stack_handler_allocator;
    stack_handler_memory_base &m_storage;
};

} // namespace async_utils

#endif
