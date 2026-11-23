#pragma once

#include "orderpulse/types.hpp"

namespace orderpulse {

// One execution report. `takerId` is the incoming (aggressing) order,
// `makerId` is the resting order it matched against; `price` is always the
// resting (maker) order's price — the maker sets the price, per standard
// price-time priority matching.
struct Trade {
    OrderId takerId = 0;
    OrderId makerId = 0;
    Price   price   = 0;
    Qty     qty     = 0;
};

} // namespace orderpulse
