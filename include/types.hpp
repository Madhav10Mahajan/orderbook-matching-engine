// Core domain types shared by the whole engine.
// Every choice here is justified in docs/04_DESIGN_DECISIONS.md (Q1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob {

// Unique order label. Billions per day -> 64 bits; never negative, no arithmetic.
using OrderId = std::uint64_t;

// Price in integer ticks (e.g. $501.37 with a $0.01 tick -> 50137).
// Exact equality for map keys; signed because some instruments trade negative.
using Price = std::int64_t;

// Shares in one order. Exchanges cap order size far below 2^31 (ITCH uses 32 bits).
// Signed so an underflow bug shows up as a negative value.
using Quantity = std::int32_t;

// Sums of many orders (level totals, traded volume) can exceed 32 bits.
using TotalQuantity = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };

inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class TimeInForce : std::uint8_t {
    GTC,  // good-till-cancel: remainder rests in the book
    IOC,  // immediate-or-cancel: fill what crosses now, discard the rest
    FOK,  // fill-or-kill: fill the whole quantity now, or do nothing at all
};

enum class Status : std::uint8_t {
    Ok,
    InvalidQuantity,  // qty <= 0
    DuplicateId,      // id already resting in the book
    UnknownId,        // cancel/modify of an id that isn't resting (filled, cancelled, never existed)
    Killed,           // FOK order could not be filled in full; nothing traded
};

// One fill between an incoming (aggressive) order and a resting order.
// Executes at the resting order's price.
struct Trade {
    OrderId buy_id;
    OrderId sell_id;
    Price price;
    Quantity qty;
};

inline bool operator==(const Trade& a, const Trade& b) {
    return a.buy_id == b.buy_id && a.sell_id == b.sell_id && a.price == b.price && a.qty == b.qty;
}

// Result of submitting an order: whether it was accepted and the fills it caused.
// `trades` only allocates when a fill happens, so non-crossing adds stay allocation-free.
struct ExecResult {
    Status status = Status::Ok;
    std::vector<Trade> trades;
    Quantity rested = 0;  // shares left resting in the book (0 for market / IOC / FOK)
};

// One row of an L2 depth snapshot.
struct LevelInfo {
    Price price;
    TotalQuantity qty;
    std::size_t order_count;
};

inline bool operator==(const LevelInfo& a, const LevelInfo& b) {
    return a.price == b.price && a.qty == b.qty && a.order_count == b.order_count;
}

}  // namespace ob
