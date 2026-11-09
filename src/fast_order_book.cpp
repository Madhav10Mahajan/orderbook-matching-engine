#include "fast_order_book.hpp"

#include <algorithm>

namespace ob {

// ---------------------------------------------------------------------------
// Price levels (sorted vector, best at back)
// ---------------------------------------------------------------------------

// lower_bound with "level is worse than price" finds the first level at least as
// good as `price`: either that exact level, or the slot where it should be inserted.
template <class Better>
auto FastOrderBook::BookSide<Better>::find(Price price) -> Level* {
    auto it = std::lower_bound(levels.begin(), levels.end(), price,
                               [](const Level& l, Price p) { return better(p, l.price); });
    return it != levels.end() && it->price == price ? &*it : nullptr;
}

template <class Better>
auto FastOrderBook::BookSide<Better>::find(Price price) const -> const Level* {
    auto it = std::lower_bound(levels.begin(), levels.end(), price,
                               [](const Level& l, Price p) { return better(p, l.price); });
    return it != levels.end() && it->price == price ? &*it : nullptr;
}

template <class Better>
auto FastOrderBook::BookSide<Better>::find_or_insert(Price price) -> Level& {
    auto it = std::lower_bound(levels.begin(), levels.end(), price,
                               [](const Level& l, Price p) { return better(p, l.price); });
    if (it != levels.end() && it->price == price) return *it;
    return *levels.insert(it, Level{price, 0, kNil, kNil, 0});
}

// ---------------------------------------------------------------------------
// Node pool
// ---------------------------------------------------------------------------

FastOrderBook::FastOrderBook(std::size_t expected_orders) : index_(expected_orders * 2) {
    nodes_.reserve(expected_orders);
    bids_.levels.reserve(1024);
    asks_.levels.reserve(1024);
}

std::uint32_t FastOrderBook::alloc_node() {
    if (free_head_ != kNil) {
        const std::uint32_t idx = free_head_;
        free_head_ = nodes_[idx].next;
        return idx;
    }
    nodes_.push_back(Node{});
    return static_cast<std::uint32_t>(nodes_.size() - 1);
}

void FastOrderBook::free_node(std::uint32_t idx) {
    nodes_[idx].next = free_head_;
    free_head_ = idx;
}

void FastOrderBook::unlink(Level& level, std::uint32_t idx) {
    const Node& n = nodes_[idx];
    if (n.prev != kNil) nodes_[n.prev].next = n.next; else level.head = n.next;
    if (n.next != kNil) nodes_[n.next].prev = n.prev; else level.tail = n.prev;
    --level.count;
}

// ---------------------------------------------------------------------------
// Order entry
// ---------------------------------------------------------------------------

ExecResult FastOrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty, TimeInForce tif) {
    ExecResult result;
    if (qty <= 0) {
        result.status = Status::InvalidQuantity;
        return result;
    }
    if (index_.contains(id)) {
        result.status = Status::DuplicateId;
        return result;
    }
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

ExecResult FastOrderBook::add_market(OrderId id, Side side, Quantity qty) {
    ExecResult result;
    if (qty <= 0) {
        result.status = Status::InvalidQuantity;
        return result;
    }
    if (index_.contains(id)) {
        result.status = Status::DuplicateId;
        return result;
    }
    Quantity remaining = qty;
    if (side == Side::Buy) {
        match(asks_, side, id, remaining, std::nullopt, result.trades);
    } else {
        match(bids_, side, id, remaining, std::nullopt, result.trades);
    }
    return result;
}

Status FastOrderBook::cancel(OrderId id) {
    const std::uint32_t idx = index_.find(id);
    if (idx == FlatIdMap::kNotFound) return Status::UnknownId;
    if (nodes_[idx].side == Side::Buy) {
        remove(bids_, idx);
    } else {
        remove(asks_, idx);
    }
    index_.erase(id);
    return Status::Ok;
}

ExecResult FastOrderBook::modify(OrderId id, Price new_price, Quantity new_qty) {
    ExecResult result;
    if (new_qty <= 0) {
        result.status = Status::InvalidQuantity;
        return result;
    }
    const std::uint32_t idx = index_.find(id);
    if (idx == FlatIdMap::kNotFound) {
        result.status = Status::UnknownId;
        return result;
    }

    Node& node = nodes_[idx];
    if (new_price == node.price && new_qty <= node.qty) {
        // Reduce in place: keeps time priority (same rule as v1).
        Level* level = node.side == Side::Buy ? bids_.find(node.price) : asks_.find(node.price);
        level->total_qty -= node.qty - new_qty;
        node.qty = new_qty;
        result.rested = new_qty;
        return result;
    }

    const Side side = node.side;
    cancel(id);
    return add_limit(id, side, new_price, new_qty, TimeInForce::GTC);
}

// ---------------------------------------------------------------------------
// Matching core (same algorithm as v1, different containers)
// ---------------------------------------------------------------------------

template <class SideBook>
void FastOrderBook::match(SideBook& opposite, Side aggressor, OrderId id, Quantity& remaining,
                          std::optional<Price> limit, std::vector<Trade>& trades) {
    auto& levels = opposite.levels;
    while (remaining > 0 && !levels.empty()) {
        Level& level = levels.back();  // best price
        if (limit && SideBook::better(*limit, level.price)) break;

        while (remaining > 0 && level.head != kNil) {
            const std::uint32_t idx = level.head;
            Node& resting = nodes_[idx];
            const Quantity fill = std::min(remaining, resting.qty);

            if (aggressor == Side::Buy) {
                trades.push_back({id, resting.id, level.price, fill});
            } else {
                trades.push_back({resting.id, id, level.price, fill});
            }

            remaining -= fill;
            resting.qty -= fill;
            level.total_qty -= fill;

            if (resting.qty == 0) {
                index_.erase(resting.id);
                unlink(level, idx);
                free_node(idx);
            }
        }

        if (level.head == kNil) levels.pop_back();
    }
}

template <class SideBook>
TotalQuantity FastOrderBook::fillable(const SideBook& opposite, Price limit, Quantity wanted) {
    TotalQuantity total = 0;
    for (auto it = opposite.levels.rbegin(); it != opposite.levels.rend() && total < wanted; ++it) {
        if (SideBook::better(limit, it->price)) break;
        total += it->total_qty;
    }
    return total;
}

template <class SideBook>
void FastOrderBook::rest(SideBook& book, Side side, OrderId id, Price price, Quantity qty) {
    const std::uint32_t idx = alloc_node();  // may grow nodes_: take references after this
    Level& level = book.find_or_insert(price);
    nodes_[idx] = Node{id, price, qty, level.tail, kNil, side};
    if (level.tail != kNil) nodes_[level.tail].next = idx; else level.head = idx;
    level.tail = idx;
    ++level.count;
    level.total_qty += qty;
    index_.insert(id, idx);
}

template <class SideBook>
void FastOrderBook::remove(SideBook& book, std::uint32_t idx) {
    Level* level = book.find(nodes_[idx].price);
    level->total_qty -= nodes_[idx].qty;
    unlink(*level, idx);
    if (level->head == kNil) book.erase(level);
    free_node(idx);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<Price> FastOrderBook::best_bid() const {
    if (bids_.levels.empty()) return std::nullopt;
    return bids_.levels.back().price;
}

std::optional<Price> FastOrderBook::best_ask() const {
    if (asks_.levels.empty()) return std::nullopt;
    return asks_.levels.back().price;
}

TotalQuantity FastOrderBook::volume_at(Side side, Price price) const {
    const Level* level = side == Side::Buy ? bids_.find(price) : asks_.find(price);
    return level ? level->total_qty : 0;
}

std::optional<Quantity> FastOrderBook::order_qty(OrderId id) const {
    const std::uint32_t idx = index_.find(id);
    if (idx == FlatIdMap::kNotFound) return std::nullopt;
    return nodes_[idx].qty;
}

std::vector<LevelInfo> FastOrderBook::depth(Side side, std::size_t n) const {
    return side == Side::Buy ? depth_of(bids_, n) : depth_of(asks_, n);
}

template <class SideBook>
std::vector<LevelInfo> FastOrderBook::depth_of(const SideBook& book, std::size_t n) {
    std::vector<LevelInfo> out;
    for (auto it = book.levels.rbegin(); it != book.levels.rend() && out.size() < n; ++it) {
        out.push_back({it->price, it->total_qty, it->count});
    }
    return out;
}

// ---------------------------------------------------------------------------
// Invariants (tests only)
// ---------------------------------------------------------------------------

std::string FastOrderBook::check_invariants() const {
    if (!bids_.levels.empty() && !asks_.levels.empty() &&
        bids_.levels.back().price >= asks_.levels.back().price) {
        return "crossed book: best bid " + std::to_string(bids_.levels.back().price) +
               " >= best ask " + std::to_string(asks_.levels.back().price);
    }
    std::size_t visited = 0;
    if (auto err = check_side(bids_, Side::Buy, visited); !err.empty()) return err;
    if (auto err = check_side(asks_, Side::Sell, visited); !err.empty()) return err;
    if (visited != index_.size()) {
        return "index has " + std::to_string(index_.size()) + " ids but book has " +
               std::to_string(visited) + " resting orders";
    }
    return "";
}

template <class SideBook>
std::string FastOrderBook::check_side(const SideBook& book, Side side, std::size_t& visited) const {
    const auto& levels = book.levels;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const Level& level = levels[i];
        const std::string where = "level " + std::to_string(level.price);
        if (i > 0 && !SideBook::better(level.price, levels[i - 1].price)) {
            return where + " out of order";
        }
        if (level.head == kNil) return where + " is empty";

        TotalQuantity sum = 0;
        std::uint32_t count = 0, prev = kNil;
        for (std::uint32_t idx = level.head; idx != kNil; idx = nodes_[idx].next) {
            if (++count > nodes_.size()) return where + ": cycle in queue";
            const Node& n = nodes_[idx];
            if (n.prev != prev) return where + ": broken prev link at order " + std::to_string(n.id);
            if (n.price != level.price || n.side != side) {
                return where + ": order " + std::to_string(n.id) + " has wrong price/side";
            }
            if (n.qty <= 0) return where + ": order " + std::to_string(n.id) + " has qty <= 0";
            if (index_.find(n.id) != idx) {
                return where + ": index entry for " + std::to_string(n.id) + " points elsewhere";
            }
            sum += n.qty;
            prev = idx;
        }
        if (prev != level.tail) return where + ": tail mismatch";
        if (count != level.count) return where + ": count mismatch";
        if (sum != level.total_qty) {
            return where + ": total_qty " + std::to_string(level.total_qty) + " != sum " + std::to_string(sum);
        }
        visited += count;
    }
    return "";
}

}  // namespace ob
