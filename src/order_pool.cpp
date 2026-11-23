#include "orderpulse/order_pool.hpp"

namespace orderpulse {

OrderPool::OrderPool(std::size_t capacity) : storage_(capacity) {
    freeList_.reserve(capacity);
    // Push in descending order so acquire() (which pops the back) hands out
    // slot 0 first — purely cosmetic, makes early debugging output readable.
    for (std::size_t i = capacity; i-- > 0;) {
        freeList_.push_back(static_cast<std::uint32_t>(i));
    }
}

Order* OrderPool::acquire() noexcept {
    if (freeList_.empty()) {
        return nullptr;
    }
    std::uint32_t idx = freeList_.back();
    freeList_.pop_back();
    Order* o = &storage_[idx];
    *o = Order{}; // reset to a clean slate
    return o;
}

void OrderPool::release(Order* o) noexcept {
    std::size_t idx = static_cast<std::size_t>(o - storage_.data());
    freeList_.push_back(static_cast<std::uint32_t>(idx));
}

} // namespace orderpulse
