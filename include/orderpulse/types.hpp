#pragma once

#include <cstdint>

namespace orderpulse {

// Prices are integer ticks, not floating point. Real exchanges price in
// integer ticks (or fixed-point) specifically to avoid floating-point
// rounding making price equality/ordering ambiguous on the matching path.
using Price   = std::int64_t;
using Qty     = std::uint32_t;
using OrderId = std::uint64_t;

// Nanosecond timestamp (or raw TSC cycles, depending on which clock the
// caller used) — the benchmark harness is the one place that cares which.
using Ts = std::uint64_t;

enum class Side : std::uint8_t { Bid, Ask };
enum class OrderType : std::uint8_t { Limit, Market };

inline constexpr Side opposite(Side s) noexcept {
    return s == Side::Bid ? Side::Ask : Side::Bid;
}

} // namespace orderpulse
