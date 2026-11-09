// Replays a LOBSTER message file (NASDAQ order-by-order data, https://lobsterdata.com)
// through the engine and, if the matching LOBSTER orderbook file is given, checks
// after EVERY message that our reconstructed book equals LOBSTER's.
//
// Usage: lobster_replay <TICKER_..._message_N.csv> [TICKER_..._orderbook_N.csv]
//
// LOBSTER message columns: time, type, order_id, size, price (dollars x 10000), direction
//   type 1 new limit order       -> add_limit
//        2 partial cancellation  -> reduce the order by `size` (keeps priority)
//        3 full deletion         -> cancel
//        4 visible execution     -> reduce the resting order by `size`, record a trade
//        5 hidden execution      -> trade only (hidden orders are not in the book)
//        6 cross trade, 7 halt   -> no book change
//   direction: 1 = buy order, -1 = sell order (for executions: the RESTING order's side)
// Orderbook columns per level: ask price, ask size, bid price, bid size (row i = state
// after message i). Empty levels use price +/-9999999999 and size 0.
//
// The file starts mid-day, so orders placed earlier are unknown to us. We seed the
// book from orderbook row 1 with one synthetic order per level, start from message 2,
// and apply events on unknown ids to the synthetic order at that price.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "fast_order_book.hpp"
#include "stats.hpp"

using namespace ob;

namespace {

constexpr Price kEmptyAsk = 9999999999LL;
constexpr Price kEmptyBid = -9999999999LL;

struct LobsterMsg {
    double time;
    int type;
    OrderId id;
    Quantity size;
    Price price;
    Side side;
};

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) out.push_back(field);
    return out;
}

bool parse_msg(const std::string& line, LobsterMsg& m) {
    auto f = split_csv(line);
    if (f.size() < 6) return false;
    m.time = std::strtod(f[0].c_str(), nullptr);
    m.type = std::atoi(f[1].c_str());
    m.id = std::strtoull(f[2].c_str(), nullptr, 10);
    m.size = static_cast<Quantity>(std::atol(f[3].c_str()));
    m.price = std::strtoll(f[4].c_str(), nullptr, 10);
    m.side = std::atoi(f[5].c_str()) == 1 ? Side::Buy : Side::Sell;
    return true;
}

// One orderbook row as (price, size) pairs per side; empty levels dropped.
struct BookRow {
    std::vector<std::pair<Price, TotalQuantity>> asks, bids;
};

bool parse_row(const std::string& line, BookRow& row) {
    auto f = split_csv(line);
    if (f.size() < 4 || f.size() % 4 != 0) return false;
    row = {};
    for (std::size_t i = 0; i + 3 < f.size(); i += 4) {
        const Price ap = std::strtoll(f[i].c_str(), nullptr, 10);
        const TotalQuantity as = std::strtoll(f[i + 1].c_str(), nullptr, 10);
        const Price bp = std::strtoll(f[i + 2].c_str(), nullptr, 10);
        const TotalQuantity bs = std::strtoll(f[i + 3].c_str(), nullptr, 10);
        if (ap != kEmptyAsk && as > 0) row.asks.push_back({ap, as});
        if (bp != kEmptyBid && bs > 0) row.bids.push_back({bp, bs});
    }
    return true;
}

class LobsterReplayer {
public:
    void seed(const BookRow& row) {
        for (auto [p, q] : row.asks) add_synthetic(Side::Sell, p, q);
        for (auto [p, q] : row.bids) add_synthetic(Side::Buy, p, q);
    }

    void apply(const LobsterMsg& m) {
        switch (m.type) {
            case 1: {
                auto r = book_.add_limit(m.id, m.side, m.price, m.size);
                if (!r.trades.empty()) ++crossing_adds_;
                if (r.status != Status::Ok) ++rejected_;
                break;
            }
            case 2: reduce(m.id, m.side, m.price, m.size); break;
            case 3: reduce(m.id, m.side, m.price, m.size, /*full=*/true); break;
            case 4:
                reduce(m.id, m.side, m.price, m.size);
                visible_.add({0, 0, m.price, m.size});
                break;
            case 5: hidden_.add({0, 0, m.price, m.size}); break;
            default: ++ignored_; break;
        }
    }

    // Compares every level present in the LOBSTER row with our depth.
    bool matches(const BookRow& row, std::size_t levels) const {
        auto cmp = [](const std::vector<LevelInfo>& ours, const std::vector<std::pair<Price, TotalQuantity>>& theirs) {
            if (ours.size() != theirs.size()) return false;  // both capped at `levels`
            for (std::size_t i = 0; i < theirs.size(); ++i) {
                if (ours[i].price != theirs[i].first || ours[i].qty != theirs[i].second) return false;
            }
            return true;
        };
        return cmp(book_.depth(Side::Sell, levels), row.asks) && cmp(book_.depth(Side::Buy, levels), row.bids);
    }

    const FastOrderBook& book() const { return book_; }
    void report(double scale) const {
        std::printf("  visible executions: %zu, volume %lld", visible_.count(),
                    static_cast<long long>(visible_.volume()));
        if (auto v = visible_.vwap()) std::printf(", VWAP %.4f", *v / scale);
        std::printf("\n  hidden executions:  %zu, volume %lld\n", hidden_.count(),
                    static_cast<long long>(hidden_.volume()));
        std::printf("  events on unknown orders: %zu, crossing adds: %zu, rejected adds: %zu, ignored (6/7): %zu\n",
                    unknown_, crossing_adds_, rejected_, ignored_);
    }

private:
    void add_synthetic(Side side, Price price, TotalQuantity qty) {
        const OrderId id = next_synthetic_++;
        book_.add_limit(id, side, price, static_cast<Quantity>(qty));
        synthetic_[{side, price}] = id;
    }

    // Shrink order `id` by `by` shares (or remove it). Same price, so modify keeps priority.
    void reduce(OrderId id, Side side, Price price, Quantity by, bool full = false) {
        auto q = book_.order_qty(id);
        if (!q) {
            // Order placed before the file started: take it out of the synthetic level order.
            auto it = synthetic_.find({side, price});
            if (it == synthetic_.end() || !book_.order_qty(it->second)) {
                ++unknown_;
                return;
            }
            id = it->second;
            q = book_.order_qty(id);
            full = false;  // deleting one old order removes only its `size` from the level
        }
        if (full || by >= *q) {
            book_.cancel(id);
        } else {
            book_.modify(id, price, *q - by);
        }
    }

    FastOrderBook book_;
    std::map<std::pair<Side, Price>, OrderId> synthetic_;
    OrderId next_synthetic_ = 1ull << 62;
    TradeStats visible_, hidden_;
    std::size_t unknown_ = 0, crossing_adds_ = 0, rejected_ = 0, ignored_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: lobster_replay <message.csv> [orderbook.csv]\n");
        return 2;
    }
    std::ifstream msg_file(argv[1]);
    if (!msg_file) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::ifstream book_file;
    const bool compare = argc > 2;
    if (compare) {
        book_file.open(argv[2]);
        if (!book_file) {
            std::fprintf(stderr, "cannot open %s\n", argv[2]);
            return 1;
        }
    }

    LobsterReplayer replayer;
    std::string mline, bline;
    std::size_t processed = 0, compared = 0, matched = 0, levels = 1;
    long first_mismatch = -1;
    LobsterMsg m{};
    BookRow row;

    if (compare) {
        // Row 1 is the state AFTER message 1: seed from it and skip message 1.
        if (!std::getline(book_file, bline) || !parse_row(bline, row) || !std::getline(msg_file, mline)) {
            std::fprintf(stderr, "empty or malformed input\n");
            return 1;
        }
        levels = split_csv(bline).size() / 4;
        replayer.seed(row);
    }

    for (long n = compare ? 2 : 1; std::getline(msg_file, mline); ++n) {
        if (!parse_msg(mline, m)) continue;
        replayer.apply(m);
        ++processed;
        if (compare && std::getline(book_file, bline) && parse_row(bline, row)) {
            ++compared;
            if (replayer.matches(row, levels)) ++matched;
            else if (first_mismatch < 0) first_mismatch = n;
        }
    }

    std::printf("LOBSTER replay: %zu messages processed\n", processed);
    replayer.report(10000.0);
    const auto& book = replayer.book();
    auto show = [](std::optional<Price> p) {
        char buf[32] = "empty";
        if (p) std::snprintf(buf, sizeof buf, "%.4f", *p / 10000.0);
        return std::string(buf);
    };
    std::printf("  final best bid %s | best ask %s | %zu resting orders\n", show(book.best_bid()).c_str(),
                show(book.best_ask()).c_str(), book.order_count());
    if (compare) {
        std::printf("Book reconstruction vs LOBSTER (%zu levels): %zu / %zu snapshots identical (%.2f%%)\n", levels,
                    matched, compared, compared ? 100.0 * matched / compared : 0.0);
        if (first_mismatch >= 0) std::printf("  first mismatch after message %ld\n", first_mismatch);
    }
    return 0;
}
