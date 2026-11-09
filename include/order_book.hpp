// v1: single-instrument limit order book with price-time priority matching,
// built from standard containers. Design and complexity: docs/04_DESIGN_DECISIONS.md.
// FastOrderBook (fast_order_book.hpp) is the optimised v2 with the same interface.
#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "types.hpp"

namespace ob {

class OrderBook {
public:
    static constexpr const char* kName = "v1 std::map + std::list";

    // Match what crosses, then GTC rests the remainder, IOC discards it,
    // FOK trades only if the whole quantity can fill now (else Status::Killed).
    ExecResult add_limit(OrderId id, Side side, Price price, Quantity qty,
                         TimeInForce tif = TimeInForce::GTC);

    // Match at any price until filled or the opposite side is empty. Never rests.
    ExecResult add_market(OrderId id, Side side, Quantity qty);

    // Remove a resting order. UnknownId if it isn't in the book.
    Status cancel(OrderId id);

    // Change a resting order's price and/or remaining quantity.
    // Same price and qty not increased -> reduced in place, keeps time priority.
    // Price change or qty increase -> cancel + re-add (back of the queue; may trade).
    ExecResult modify(OrderId id, Price new_price, Quantity new_qty);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    TotalQuantity volume_at(Side side, Price price) const;
    std::vector<LevelInfo> depth(Side side, std::size_t n) const;
    std::optional<Quantity> order_qty(OrderId id) const;
    std::size_t order_count() const { return orders_by_id_.size(); }

    // Checks every invariant from docs/01_PROBLEM_STATEMENT.md §5.
    // Returns an empty string if the book is consistent, else a description of the first problem.
    std::string check_invariants() const;

private:
    struct Order {
        OrderId id;
        Quantity qty;
    };
    using Queue = std::list<Order>;  // FIFO: front = oldest = first to fill

    struct Level {
        TotalQuantity total_qty = 0;
        Queue orders;
    };

    // Where a resting order lives, so cancel never scans the book.
    struct Location {
        Side side;
        Price price;
        Queue::iterator it;  // std::list iterators survive other inserts/erases
    };

    // begin() is the best price on both sides: lowest ask, highest bid.
    using AskMap = std::map<Price, Level>;
    using BidMap = std::map<Price, Level, std::greater<Price>>;

    template <class Book>
    void match(Book& opposite, Side aggressor, OrderId id, Quantity& remaining,
               std::optional<Price> limit, std::vector<Trade>& trades);

    template <class Book>
    static TotalQuantity fillable(const Book& opposite, Price limit, Quantity wanted);

    template <class Book>
    void rest(Book& book, Side side, OrderId id, Price price, Quantity qty);

    template <class Book>
    void remove(Book& book, const Location& loc);

    template <class Book>
    static std::vector<LevelInfo> depth_of(const Book& book, std::size_t n);

    template <class Book>
    std::string check_side(const Book& book, Side side) const;

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Location> orders_by_id_;
};

}  // namespace ob
