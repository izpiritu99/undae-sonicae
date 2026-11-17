// A shared_ptr several threads swap safely (std::atomic<std::shared_ptr> is not in every standard library: Android's
// libc++ lacks it). A spin lock held only for a pointer copy, so the audio thread can use it.
#pragma once
#include <atomic>
#include <memory>

namespace us {

template <class T> class AtomicShared {
public:
    AtomicShared() = default;
    AtomicShared(std::shared_ptr<T> v) : p(std::move(v)) {}
    std::shared_ptr<T> load() const { lock(); auto c = p; unlock(); return c; }
    void store(std::shared_ptr<T> v) { lock(); p.swap(v); unlock(); }   // the old value is released after unlocking
    std::shared_ptr<T> exchange(std::shared_ptr<T> v) { lock(); p.swap(v); unlock(); return v; }
private:
    mutable std::atomic_flag f = ATOMIC_FLAG_INIT; std::shared_ptr<T> p;
    void lock() const { while (f.test_and_set(std::memory_order_acquire)) {} }
    void unlock() const { f.clear(std::memory_order_release); }
};

}   // namespace us
