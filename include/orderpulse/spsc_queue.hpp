#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace orderpulse {

// 64 bytes covers essentially every x86-64 and current ARM64 target.
// std::hardware_destructive_interference_size exists for exactly this, but
// its value depends on -mtune/-mcpu at compile time, which would make this
// queue's layout (and therefore ABI) silently vary by build flags — not
// something to leave implicit for a struct users might share across
// translation units built differently. A pinned constant is the safer call.
inline constexpr std::size_t kCacheLineSize = 64;

// Single-producer/single-consumer lock-free ring buffer. This is the
// wait-free hot path connecting the feed-simulator thread (producer) to the
// matching-engine thread (consumer) — see README.md ("Concurrency design")
// for why SPSC rather than a mutex-guarded queue or a general MPMC queue:
// the real topology is exactly one feed handler and exactly one matching
// core, so a plain load/store protocol with one atomic per side is both
// sufficient and strictly cheaper than anything using a CAS loop or a lock.
//
// T must be trivially copyable — events cross the thread boundary by value,
// never by pointer, so there is no ownership to reason about and no
// allocation on either side of the hot path.
template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t minCapacity) : capacity_(roundUpPow2(minCapacity)), mask_(capacity_ - 1), buffer_(capacity_) {}

    // Producer side, called only from the producer thread. Returns false if
    // the queue is full — the caller decides how to react (spin, drop,
    // apply backpressure); this class never blocks.
    bool tryPush(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            return false; // full
        }
        buffer_[head & mask_] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side, called only from the consumer thread. Returns false if
    // the queue is empty.
    bool tryPop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false; // empty
        }
        out = buffer_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    static std::size_t roundUpPow2(std::size_t v) noexcept {
        std::size_t p = 1;
        while (p < v) {
            p <<= 1;
        }
        return p;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<T> buffer_;

    // head_ and tail_ are each pinned to their own cache line: head_ is
    // written only by the producer and read by the consumer, tail_ is
    // written only by the consumer and read by the producer. Without this
    // padding the two atomics would very likely share a cache line, and
    // every push/pop pair would ping-pong that line between cores (false
    // sharing) even though the two threads never touch the same logical
    // data.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
};

} // namespace orderpulse
