// Synthetic order flow with the rough shape of real exchange traffic, used by the
// benchmark and by `gen_orders` to make replay files.
//
// Stylised facts it imitates:
//   - adds and cancels dominate; trades are a small fraction of messages
//   - most limit orders land within a few ticks of the best price
//     (distance from the touch is roughly geometric)
//   - a minority of orders cross the spread and trade immediately
//   - the "fair price" drifts as a random walk
//
// Default mix: 50% passive adds, 6% marketable adds (some IOC), 34% cancels,
// 6% modifies, 4% market orders.
#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "message.hpp"

namespace ob {

class OrderFlowGenerator {
public:
    explicit OrderFlowGenerator(std::uint64_t seed, Price start_mid = 10000)
        : rng_(seed), mid_(start_mid) {}

    Message next() {
        if (++count_ % 64 == 0) mid_ += static_cast<Price>(roll(3)) - 1;  // random walk

        const int r = roll(100);
        if (r < 50 || live_.empty()) return passive_add();
        if (r < 56) return marketable_add();
        if (r < 90) return cancel();
        if (r < 96) return modify();
        return market();
    }

private:
    int roll(int n) { return static_cast<int>(rng_() % static_cast<std::uint64_t>(n)); }
    Side side() { return roll(2) ? Side::Buy : Side::Sell; }
    Quantity qty() { return static_cast<Quantity>(1 + roll(10)) * (roll(4) == 0 ? 100 : 10); }

    // Ticks away from the touch: P(d) ~ 0.7^d, capped. Most orders sit near the top.
    Price distance() {
        Price d = 0;
        while (d < 60 && roll(10) < 7) ++d;
        return d;
    }

    Message passive_add() {
        Message m;
        m.type = MsgType::Add;
        m.id = next_id_++;
        m.side = side();
        m.price = m.side == Side::Buy ? mid_ - 1 - distance() : mid_ + 1 + distance();
        m.qty = qty();
        live_.push_back(m.id);
        return m;
    }

    Message marketable_add() {
        Message m;
        m.type = MsgType::Add;
        m.id = next_id_++;
        m.side = side();
        const Price through = roll(3);  // willing to pay up to a few ticks through mid
        m.price = m.side == Side::Buy ? mid_ + through : mid_ - through;
        m.qty = qty();
        m.tif = roll(3) == 0 ? TimeInForce::IOC : TimeInForce::GTC;
        if (m.tif == TimeInForce::GTC) live_.push_back(m.id);
        return m;
    }

    // Cancels pick a random id we placed. It may already have been filled, in which
    // case the book answers UnknownId — real feeds see that race too.
    Message cancel() {
        Message m;
        m.type = MsgType::Cancel;
        const std::size_t i = rng_() % live_.size();
        m.id = live_[i];
        live_[i] = live_.back();
        live_.pop_back();
        return m;
    }

    Message modify() {
        Message m;
        m.type = MsgType::Modify;
        m.id = live_[rng_() % live_.size()];
        m.side = side();  // unused by modify; kept for completeness
        m.price = mid_ + (roll(2) ? 1 + distance() : -1 - distance());
        m.qty = qty();
        return m;
    }

    Message market() {
        Message m;
        m.type = MsgType::Market;
        m.id = next_id_++;
        m.side = side();
        m.qty = qty();
        return m;
    }

    std::mt19937_64 rng_;
    Price mid_;
    OrderId next_id_ = 1;
    std::uint64_t count_ = 0;
    std::vector<OrderId> live_;
};

}  // namespace ob
