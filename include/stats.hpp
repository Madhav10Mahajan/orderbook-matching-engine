// Market-microstructure statistics computed from the book and the trade tape.
// All functions work with any book type that has the common query interface.
// Prices are in ticks; convert for display.
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>

#include "types.hpp"

namespace ob {

// best ask - best bid. The cost of a round trip (buy then sell immediately).
template <class Book>
std::optional<Price> spread(const Book& book) {
    auto bid = book.best_bid();
    auto ask = book.best_ask();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}

// (best bid + best ask) / 2. The usual "fair price" reference.
template <class Book>
std::optional<double> mid_price(const Book& book) {
    auto bid = book.best_bid();
    auto ask = book.best_ask();
    if (!bid || !ask) return std::nullopt;
    return (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0;
}

// Order-book imbalance over the top `levels` levels:
//   (bid qty - ask qty) / (bid qty + ask qty), in [-1, 1].
// Positive = more resting buy interest; empirically a short-horizon predictor of
// the next mid-price move.
template <class Book>
std::optional<double> imbalance(const Book& book, std::size_t levels = 1) {
    TotalQuantity bid_qty = 0, ask_qty = 0;
    for (const auto& l : book.depth(Side::Buy, levels)) bid_qty += l.qty;
    for (const auto& l : book.depth(Side::Sell, levels)) ask_qty += l.qty;
    if (bid_qty + ask_qty == 0) return std::nullopt;
    return static_cast<double>(bid_qty - ask_qty) / static_cast<double>(bid_qty + ask_qty);
}

// Size-weighted mid ("microprice"): leans toward the side with LESS size, because
// that side is more likely to be consumed next.
//   (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)
template <class Book>
std::optional<double> microprice(const Book& book) {
    auto bids = book.depth(Side::Buy, 1);
    auto asks = book.depth(Side::Sell, 1);
    if (bids.empty() || asks.empty()) return std::nullopt;
    const double bq = static_cast<double>(bids[0].qty), aq = static_cast<double>(asks[0].qty);
    return (static_cast<double>(bids[0].price) * aq + static_cast<double>(asks[0].price) * bq) / (bq + aq);
}

// Running statistics over the trade tape.
class TradeStats {
public:
    void add(const Trade& t) {
        if (count_ == 0) high_ = low_ = t.price;
        ++count_;
        volume_ += t.qty;
        notional_ += static_cast<double>(t.price) * static_cast<double>(t.qty);
        high_ = std::max(high_, t.price);
        low_ = std::min(low_, t.price);
        last_ = t.price;
    }

    std::size_t count() const { return count_; }
    TotalQuantity volume() const { return volume_; }
    // Volume-weighted average price: sum(price * qty) / sum(qty).
    std::optional<double> vwap() const {
        if (volume_ == 0) return std::nullopt;
        return notional_ / static_cast<double>(volume_);
    }
    Price high() const { return high_; }
    Price low() const { return low_; }
    Price last() const { return last_; }

private:
    std::size_t count_ = 0;
    TotalQuantity volume_ = 0;
    double notional_ = 0.0;
    Price high_ = 0, low_ = 0, last_ = 0;
};

}  // namespace ob
