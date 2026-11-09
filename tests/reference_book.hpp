// Reference model: the slowest, most obviously-correct order book we can write.
// One flat vector of orders, every operation is a linear scan. No maps, no
// iterators, no bookkeeping that can drift. The fuzzer runs it side by side with
// the real books and demands identical output for every message.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "types.hpp"

namespace ob {

class ReferenceBook {
public:
    static constexpr const char* kName = "reference";

    ExecResult add_limit(OrderId id, Side side, Price price, Quantity qty,
                         TimeInForce tif = TimeInForce::GTC) {
        return submit(id, side, price, qty, tif, /*is_market=*/false);
    }

    ExecResult add_market(OrderId id, Side side, Quantity qty) {
        return submit(id, side, 0, qty, TimeInForce::IOC, /*is_market=*/true);
    }

    Status cancel(OrderId id) {
        auto it = find(id);
        if (it == orders_.end()) return Status::UnknownId;
        orders_.erase(it);
        return Status::Ok;
    }

    ExecResult modify(OrderId id, Price new_price, Quantity new_qty) {
        ExecResult r;
        if (new_qty <= 0) {
            r.status = Status::InvalidQuantity;
            return r;
        }
        auto it = find(id);
        if (it == orders_.end()) {
            r.status = Status::UnknownId;
            return r;
        }
        if (new_price == it->price && new_qty <= it->qty) {
            it->qty = new_qty;  // keeps its sequence number = keeps priority
            r.rested = new_qty;
            return r;
        }
        const Side side = it->side;
        orders_.erase(it);
        return add_limit(id, side, new_price, new_qty, TimeInForce::GTC);
    }

    std::optional<Price> best_bid() const { return best(Side::Buy); }
    std::optional<Price> best_ask() const { return best(Side::Sell); }

    TotalQuantity volume_at(Side side, Price price) const {
        TotalQuantity total = 0;
        for (const auto& o : orders_)
            if (o.side == side && o.price == price) total += o.qty;
        return total;
    }

    std::vector<LevelInfo> depth(Side side, std::size_t n) const {
        std::map<Price, LevelInfo> levels;
        for (const auto& o : orders_) {
            if (o.side != side) continue;
            auto& l = levels[o.price];
            l.price = o.price;
            l.qty += o.qty;
            l.order_count += 1;
        }
        std::vector<LevelInfo> out;
        for (const auto& [p, l] : levels) out.push_back(l);
        if (side == Side::Buy) std::reverse(out.begin(), out.end());  // highest bid first
        if (out.size() > n) out.resize(n);
        return out;
    }

    std::optional<Quantity> order_qty(OrderId id) const {
        auto it = std::find_if(orders_.begin(), orders_.end(), [&](const RefOrder& o) { return o.id == id; });
        if (it == orders_.end()) return std::nullopt;
        return it->qty;
    }

    std::size_t order_count() const { return orders_.size(); }
    std::string check_invariants() const { return ""; }

private:
    struct RefOrder {
        OrderId id;
        Side side;
        Price price;
        Quantity qty;
        std::uint64_t seq;  // arrival order: smaller = earlier
    };

    std::vector<RefOrder>::iterator find(OrderId id) {
        return std::find_if(orders_.begin(), orders_.end(), [&](const RefOrder& o) { return o.id == id; });
    }

    std::optional<Price> best(Side side) const {
        std::optional<Price> b;
        for (const auto& o : orders_) {
            if (o.side != side) continue;
            if (!b || (side == Side::Buy ? o.price > *b : o.price < *b)) b = o.price;
        }
        return b;
    }

    // Can a resting order at `resting_price` trade with an incoming order?
    static bool crosses(Side incoming, Price limit, bool is_market, Price resting_price) {
        if (is_market) return true;
        return incoming == Side::Buy ? resting_price <= limit : resting_price >= limit;
    }

    // The resting order that should fill next: best price, then earliest arrival.
    std::vector<RefOrder>::iterator next_to_fill(Side incoming, Price limit, bool is_market) {
        auto best_it = orders_.end();
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (it->side == incoming || !crosses(incoming, limit, is_market, it->price)) continue;
            if (best_it == orders_.end()) {
                best_it = it;
                continue;
            }
            const bool better_price =
                incoming == Side::Buy ? it->price < best_it->price : it->price > best_it->price;
            if (better_price || (it->price == best_it->price && it->seq < best_it->seq)) best_it = it;
        }
        return best_it;
    }

    ExecResult submit(OrderId id, Side side, Price price, Quantity qty, TimeInForce tif, bool is_market) {
        ExecResult r;
        if (qty <= 0) {
            r.status = Status::InvalidQuantity;
            return r;
        }
        if (find(id) != orders_.end()) {
            r.status = Status::DuplicateId;
            return r;
        }
        if (tif == TimeInForce::FOK) {
            TotalQuantity available = 0;
            for (const auto& o : orders_)
                if (o.side != side && crosses(side, price, is_market, o.price)) available += o.qty;
            if (available < qty) {
                r.status = Status::Killed;
                return r;
            }
        }

        Quantity remaining = qty;
        while (remaining > 0) {
            auto it = next_to_fill(side, price, is_market);
            if (it == orders_.end()) break;
            const Quantity fill = std::min(remaining, it->qty);
            if (side == Side::Buy) {
                r.trades.push_back({id, it->id, it->price, fill});
            } else {
                r.trades.push_back({it->id, id, it->price, fill});
            }
            remaining -= fill;
            it->qty -= fill;
            if (it->qty == 0) orders_.erase(it);
        }

        if (remaining > 0 && tif == TimeInForce::GTC && !is_market) {
            orders_.push_back({id, side, price, remaining, next_seq_++});
            r.rested = remaining;
        }
        return r;
    }

    std::vector<RefOrder> orders_;
    std::uint64_t next_seq_ = 0;
};

}  // namespace ob
