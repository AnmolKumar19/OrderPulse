#pragma once

#include <cstdint>
#include <vector>

#include "orderpulse/order.hpp"

namespace orderpulse {

// Fixed-capacity pool of Order slots, indexed by a freelist of slot indices.
// acquire()/release() never call new/delete and never invalidate previously
// acquired Order* (the backing storage is allocated once, up front, and
// never reallocated) — that's the point: the matching engine's hot path
// does zero heap allocation.
class OrderPool {
public:
    explicit OrderPool(std::size_t capacity);

    // Returns a pointer to a zeroed-out Order slot, or nullptr if the pool
    // is exhausted (capacity is a hard cap — the caller decides how to
    // react, e.g. reject the incoming order).
    Order* acquire() noexcept;

    // Returns a slot to the freelist. `o` must have come from this pool via
    // acquire() and must not be used again after this call.
    void release(Order* o) noexcept;

    std::size_t capacity() const noexcept { return storage_.size(); }
    std::size_t inUse() const noexcept { return storage_.size() - freeList_.size(); }

private:
    std::vector<Order> storage_;
    std::vector<std::uint32_t> freeList_; // indices into storage_, LIFO
};

} // namespace orderpulse
