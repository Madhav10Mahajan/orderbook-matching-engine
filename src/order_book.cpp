#include "order_book.hpp"

#include <algorithm>
#include <iterator>

namespace ob {

// ---------------------------------------------------------------------------
// Order entry
// ---------------------------------------------------------------------------

ExecResult OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty, TimeInForce tif) {
    ExecResult result;
    if (qty <= 0) {
        result.status = Status::InvalidQuantity;
        return result;
    }
    if (orders_by_id_.count(id)) {
        result.status = Status::DuplicateId;
        return result;
    }
    // FOK must know BEFORE trading whether the whole order can fill, because
    // trades can't be undone. Checking costs one walk over the crossing levels.
    if (tif == TimeInForce::FOK) {
        const TotalQuantity available =
            side == Side::Buy ? fillable(asks_, price, qty) : fillable(bids_, price, qty);
        if (available < qty) {
            result.status = Status::Killed;
            return result;
        }
    }

    Quantity remaining = qty;
    if (side == Side::Buy) {
        match(asks_, side, id, remaining, price, result.trades);
        if (remaining > 0 && tif == TimeInForce::GTC) rest(bids_, side, id, price, remaining);
    } else {
        match(bids_, side, id, remaining, price, result.trades);
        if (remaining > 0 && tif == TimeInForce::GTC) rest(asks_, side, id, price, remaining);
    }
    result.rested = tif == TimeInForce::GTC ? remaining : 0;
    return result;
}

ExecResult OrderBook::add_market(OrderId id, Side side, Quantity qty) {
    ExecResult result;
    if (qty <= 0) {
        result.status = Status::InvalidQuantity;
        return result;
    }
    // A market order never rests, but reusing a resting id would make trade reports ambiguous.
    if (orders_by_id_.count(id)) {
        result.status = Status::DuplicateId;
        return result;
    }

    Quantity remaining = qty;
    if (side == Side::Buy) {
        match(asks_, side, id, remaining, std::nullopt, result.trades);
    } else {
        match(bids_, side, id, remaining, std::nullopt, result.trades);
    }
    // Any unfilled remainder is discarded: market orders don't rest.
    return result;
}

Status OrderBook::cancel(OrderId id) {
    auto found = orders_by_id_.find(id);  // O(1): the catalogue, not a scan
    if (found == orders_by_id_.end()) return Status::UnknownId;

    const Location loc = found->second;
    if (loc.side == Side::Buy) {
        remove(bids_, loc);
    } else {
        remove(asks_, loc);
    }
    orders_by_id_.erase(found);
    return Status::Ok;
}

ExecResult OrderBook::modify(OrderId id, Price new_price, Quantity new_qty) {
    ExecResult result;
    if (new_qty <= 0) {
        result.status = Status::InvalidQuantity;  // use cancel() to remove an order
        return result;
    }
    auto found = orders_by_id_.find(id);
    if (found == orders_by_id_.end()) {
        result.status = Status::UnknownId;
        return result;
    }

    Location& loc = found->second;
    const Quantity current = loc.it->qty;
    if (new_price == loc.price && new_qty <= current) {
        // Shrinking in place keeps the order's place in the queue: it only
        // makes the order less aggressive, so nobody behind it is disadvantaged.
        Level& level = loc.side == Side::Buy ? bids_.find(loc.price)->second
                                             : asks_.find(loc.price)->second;
        level.total_qty -= current - new_qty;
        loc.it->qty = new_qty;
        result.rested = new_qty;
        return result;
    }

    // Anything else would let the order jump the queue, so it loses priority:
    // cancel and re-submit (the new price may cross and trade).
    const Side side = loc.side;
    cancel(id);
    return add_limit(id, side, new_price, new_qty, TimeInForce::GTC);
}

// ---------------------------------------------------------------------------
// Matching core
// ---------------------------------------------------------------------------

// Fill `remaining` against `opposite`, best price first, oldest order first within a price.
// `limit` = nullopt means a market order (crosses every level).
//
// The same code handles buys (opposite = asks, ascending) and sells (opposite = bids,
// descending) because key_comp() means "is a better price than" on that side:
//   asks: better(a, b) = a < b      bids: better(a, b) = a > b
// A level is tradable unless our limit is strictly better than it, e.g. a buy at 500
// vs. an ask at 501: better(500, 501) is true -> stop.
template <class Book>
void OrderBook::match(Book& opposite, Side aggressor, OrderId id, Quantity& remaining,
                      std::optional<Price> limit, std::vector<Trade>& trades) {
    const auto better = opposite.key_comp();

    while (remaining > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();  // best price: O(1)
        const Price level_price = level_it->first;
        if (limit && better(*limit, level_price)) break;  // no longer crosses

        Level& level = level_it->second;
        while (remaining > 0 && !level.orders.empty()) {
            Order& resting = level.orders.front();  // time priority: oldest first
            const Quantity fill = std::min(remaining, resting.qty);

            // Trade prints at the RESTING order's price.
            if (aggressor == Side::Buy) {
                trades.push_back({id, resting.id, level_price, fill});
            } else {
                trades.push_back({resting.id, id, level_price, fill});
            }

            remaining -= fill;
            resting.qty -= fill;
            level.total_qty -= fill;

            if (resting.qty == 0) {
                // Leaving the book -> leave the catalogue too, or a later cancel
                // would follow a dangling iterator.
                orders_by_id_.erase(resting.id);
                level.orders.pop_front();
            }
        }

        if (level.orders.empty()) opposite.erase(level_it);  // no empty levels
    }
}

// Shares available at prices that cross `limit`, stopping early once `wanted` is reached.
template <class Book>
TotalQuantity OrderBook::fillable(const Book& opposite, Price limit, Quantity wanted) {
    const auto better = opposite.key_comp();
    TotalQuantity total = 0;
    for (auto it = opposite.begin(); it != opposite.end() && total < wanted; ++it) {
        if (better(limit, it->first)) break;
        total += it->second.total_qty;
    }
    return total;
}

template <class Book>
void OrderBook::rest(Book& book, Side side, OrderId id, Price price, Quantity qty) {
    Level& level = book[price];  // O(log M); creates the level if it's a new price
    level.orders.push_back({id, qty});
    level.total_qty += qty;
    orders_by_id_.emplace(id, Location{side, price, std::prev(level.orders.end())});
}

template <class Book>
void OrderBook::remove(Book& book, const Location& loc) {
    auto level_it = book.find(loc.price);  // O(log M)
    Level& level = level_it->second;
    level.total_qty -= loc.it->qty;
    level.orders.erase(loc.it);  // O(1): unlink the node, nothing else moves
    if (level.orders.empty()) book.erase(level_it);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

TotalQuantity OrderBook::volume_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_qty;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_qty;
}

std::optional<Quantity> OrderBook::order_qty(OrderId id) const {
    auto found = orders_by_id_.find(id);
    if (found == orders_by_id_.end()) return std::nullopt;
    return found->second.it->qty;
}

std::vector<LevelInfo> OrderBook::depth(Side side, std::size_t n) const {
    return side == Side::Buy ? depth_of(bids_, n) : depth_of(asks_, n);
}

template <class Book>
std::vector<LevelInfo> OrderBook::depth_of(const Book& book, std::size_t n) {
    std::vector<LevelInfo> out;
    for (auto it = book.begin(); it != book.end() && out.size() < n; ++it) {
        out.push_back({it->first, it->second.total_qty, it->second.orders.size()});
    }
    return out;
}

// ---------------------------------------------------------------------------
// Invariants (used by tests; O(N), never on the hot path)
// ---------------------------------------------------------------------------

std::string OrderBook::check_invariants() const {
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
        return "crossed book: best bid " + std::to_string(bids_.begin()->first) +
               " >= best ask " + std::to_string(asks_.begin()->first);
    }
    if (auto err = check_side(bids_, Side::Buy); !err.empty()) return err;
    if (auto err = check_side(asks_, Side::Sell); !err.empty()) return err;

    std::size_t resting = 0;
    for (const auto& [price, level] : bids_) resting += level.orders.size();
    for (const auto& [price, level] : asks_) resting += level.orders.size();
    // Every resting order was matched to its own catalogue entry above, so equal
    // counts mean the catalogue holds nothing extra (no stale ids).
    if (resting != orders_by_id_.size()) {
        return "catalogue has " + std::to_string(orders_by_id_.size()) + " ids but book has " +
               std::to_string(resting) + " resting orders";
    }
    return "";
}

template <class Book>
std::string OrderBook::check_side(const Book& book, Side side) const {
    for (const auto& [price, level] : book) {
        const std::string where = "level " + std::to_string(price);
        if (level.orders.empty()) return where + " is empty";

        TotalQuantity sum = 0;
        for (const Order& order : level.orders) {
            if (order.qty <= 0) return where + ": order " + std::to_string(order.id) + " has qty <= 0";
            sum += order.qty;

            auto found = orders_by_id_.find(order.id);
            if (found == orders_by_id_.end()) {
                return where + ": order " + std::to_string(order.id) + " missing from catalogue";
            }
            const Location& loc = found->second;
            if (loc.side != side || loc.price != price || &*loc.it != &order) {
                return where + ": catalogue entry for " + std::to_string(order.id) + " points elsewhere";
            }
        }
        if (sum != level.total_qty) {
            return where + ": total_qty " + std::to_string(level.total_qty) + " != sum " + std::to_string(sum);
        }
    }
    return "";
}

}  // namespace ob
