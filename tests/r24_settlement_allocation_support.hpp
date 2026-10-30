#pragma once

// Used by exactly one regression translation unit. No production allocator or
// public runtime API is changed. All paths still use malloc/free (or the POSIX
// aligned equivalent), so normal process exit permits ASan/LSan inspection.
#include <cstddef>
#include <cstdlib>
#include <new>

namespace r24_allocation_test {
struct Probe {
    bool measuring{};
    bool fail_next{};
    std::size_t allocations{};
    std::size_t first_size{};
    std::size_t failures{};
    std::size_t failed_size{};
};
inline thread_local Probe probe;

inline void before_allocate(std::size_t size) {
    if (probe.measuring) {
        ++probe.allocations;
        if (probe.allocations == 1) probe.first_size = size;
    }
    if (probe.fail_next) {
        probe.fail_next = false;
        ++probe.failures;
        probe.failed_size = size;
        throw std::bad_alloc();
    }
}

[[gnu::noinline]] inline void* allocate(std::size_t size) {
    before_allocate(size);
    for (;;) {
        if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

[[gnu::noinline]] inline void* allocate_aligned(std::size_t size, std::align_val_t alignment) {
    before_allocate(size);
    for (;;) {
        void* memory = nullptr;
        if (::posix_memalign(&memory, static_cast<std::size_t>(alignment), size == 0 ? 1 : size) == 0)
            return memory;
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

[[gnu::noinline]] inline void release(void* memory) noexcept { std::free(memory); }
} // namespace r24_allocation_test

[[gnu::noinline]] void* operator new(std::size_t size) { return r24_allocation_test::allocate(size); }
[[gnu::noinline]] void* operator new[](std::size_t size) { return r24_allocation_test::allocate(size); }
void operator delete(void* memory) noexcept { r24_allocation_test::release(memory); }
void operator delete[](void* memory) noexcept { r24_allocation_test::release(memory); }
void operator delete(void* memory, std::size_t) noexcept { r24_allocation_test::release(memory); }
void operator delete[](void* memory, std::size_t) noexcept { r24_allocation_test::release(memory); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return r24_allocation_test::allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return r24_allocation_test::allocate(size); } catch (...) { return nullptr; }
}
void operator delete(void* memory, const std::nothrow_t&) noexcept { r24_allocation_test::release(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { r24_allocation_test::release(memory); }

void* operator new(std::size_t size, std::align_val_t alignment) {
    return r24_allocation_test::allocate_aligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return r24_allocation_test::allocate_aligned(size, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept { r24_allocation_test::release(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { r24_allocation_test::release(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { r24_allocation_test::release(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { r24_allocation_test::release(memory); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return r24_allocation_test::allocate_aligned(size, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return r24_allocation_test::allocate_aligned(size, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept { r24_allocation_test::release(memory); }
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept { r24_allocation_test::release(memory); }
