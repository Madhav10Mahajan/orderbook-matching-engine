// Differential fuzzer: feeds the same random message stream to a real book and to
// ReferenceBook, and stops at the first message where ANY observable output differs
// (status, trades, rested qty, BBO, depth, order count) or an invariant breaks.
//
// The stream deliberately includes edge cases: duplicate ids, cancels/modifies of
// dead ids, zero/negative quantities, FOK orders that can't fill, market orders
// into an empty side, and a narrow price band so most orders interact.
//
// Usage: fuzz [steps_per_seed=200000] [seeds=5] [first_seed=1]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "fast_order_book.hpp"
#include "order_book.hpp"
#include "reference_book.hpp"

using namespace ob;

namespace {

std::string fmt(const ExecResult& r) {
    std::string s = "status=" + std::to_string(static_cast<int>(r.status)) +
                    " rested=" + std::to_string(r.rested) + " trades=[";
    for (const Trade& t : r.trades) {
        s += "(" + std::to_string(t.buy_id) + "," + std::to_string(t.sell_id) + "," +
             std::to_string(t.price) + "," + std::to_string(t.qty) + ")";
    }
    return s + "]";
}

std::string fmt(const std::vector<LevelInfo>& d) {
    std::string s;
    for (const auto& l : d) {
        s += std::to_string(l.price) + ":" + std::to_string(l.qty) + "/" + std::to_string(l.order_count) + " ";
    }
    return s;
}

std::string fmt(std::optional<Price> p) { return p ? std::to_string(*p) : "none"; }

bool same(const ExecResult& a, const ExecResult& b) {
    return a.status == b.status && a.rested == b.rested && a.trades == b.trades;
}

template <class Book>
bool run_seed(std::uint64_t seed, long steps) {
    Book book;
    ReferenceBook ref;
    std::mt19937_64 rng(seed);
    auto roll = [&](int n) { return static_cast<int>(rng() % static_cast<std::uint64_t>(n)); };

    std::vector<OrderId> ids;  // every id ever used (alive or dead)
    std::unordered_map<OrderId, Price> last_price;
    OrderId next_id = 1;
    const Price lo = 1000, hi = 1030;  // ~30 ticks: lots of crossing and queueing

    auto pick_id = [&]() -> OrderId {
        if (ids.empty() || roll(20) == 0) return next_id + 1000000;  // never-seen id
        return ids[rng() % ids.size()];
    };
    auto rand_qty = [&]() -> Quantity { return roll(50) == 0 ? -roll(3) : 1 + roll(60); };
    auto rand_price = [&]() -> Price { return lo + roll(static_cast<int>(hi - lo + 1)); };

    for (long step = 0; step < steps; ++step) {
        std::string op;
        bool ok = true;
        std::string detail;
        const int r = roll(100);
        const Side side = roll(2) ? Side::Buy : Side::Sell;

        if (r < 45) {
            const OrderId id = (roll(20) == 0 && !ids.empty()) ? pick_id() : next_id++;
            const Price p = rand_price();
            const Quantity q = rand_qty();
            const int t = roll(10);
            const TimeInForce tif = t < 8 ? TimeInForce::GTC : (t == 8 ? TimeInForce::IOC : TimeInForce::FOK);
            op = "add_limit id=" + std::to_string(id) + " side=" + std::to_string(static_cast<int>(side)) +
                 " px=" + std::to_string(p) + " qty=" + std::to_string(q) + " tif=" + std::to_string(static_cast<int>(tif));
            auto a = book.add_limit(id, side, p, q, tif);
            auto b = ref.add_limit(id, side, p, q, tif);
            ok = same(a, b);
            detail = "book " + fmt(a) + "\n    ref  " + fmt(b);
            ids.push_back(id);
            last_price[id] = p;
        } else if (r < 72) {
            const OrderId id = pick_id();
            op = "cancel id=" + std::to_string(id);
            auto a = book.cancel(id);
            auto b = ref.cancel(id);
            ok = a == b;
            detail = "book " + std::to_string(static_cast<int>(a)) + " ref " + std::to_string(static_cast<int>(b));
        } else if (r < 85) {
            const OrderId id = pick_id();
            // Half the time keep the price (exercises the keep-priority path).
            const Price p = (roll(2) && last_price.count(id)) ? last_price[id] : rand_price();
            const Quantity q = rand_qty();
            op = "modify id=" + std::to_string(id) + " px=" + std::to_string(p) + " qty=" + std::to_string(q);
            auto a = book.modify(id, p, q);
            auto b = ref.modify(id, p, q);
            ok = same(a, b);
            detail = "book " + fmt(a) + "\n    ref  " + fmt(b);
            if (a.status == Status::Ok) last_price[id] = p;
        } else {
            const OrderId id = next_id++;
            const Quantity q = roll(50) == 0 ? 0 : 1 + roll(150);
            op = "market id=" + std::to_string(id) + " side=" + std::to_string(static_cast<int>(side)) +
                 " qty=" + std::to_string(q);
            auto a = book.add_market(id, side, q);
            auto b = ref.add_market(id, side, q);
            ok = same(a, b);
            detail = "book " + fmt(a) + "\n    ref  " + fmt(b);
        }

        // Compare the whole visible state after every message.
        if (ok && (book.best_bid() != ref.best_bid() || book.best_ask() != ref.best_ask())) {
            ok = false;
            detail = "BBO book " + fmt(book.best_bid()) + "/" + fmt(book.best_ask()) + " ref " +
                     fmt(ref.best_bid()) + "/" + fmt(ref.best_ask());
        }
        if (ok && book.order_count() != ref.order_count()) {
            ok = false;
            detail = "order_count book " + std::to_string(book.order_count()) + " ref " +
                     std::to_string(ref.order_count());
        }
        if (ok) {
            for (Side s : {Side::Buy, Side::Sell}) {
                auto a = book.depth(s, 64);
                auto b = ref.depth(s, 64);
                if (a != b) {
                    ok = false;
                    detail = "depth side " + std::to_string(static_cast<int>(s)) + "\n    book " + fmt(a) +
                             "\n    ref  " + fmt(b);
                    break;
                }
            }
        }
        if (ok) {
            if (auto err = book.check_invariants(); !err.empty()) {
                ok = false;
                detail = "invariant: " + err;
            }
        }
        if (!ok) {
            std::printf("  MISMATCH [%s] seed=%llu step=%ld\n    op: %s\n    %s\n", Book::kName,
                        static_cast<unsigned long long>(seed), step, op.c_str(), detail.c_str());
            return false;
        }
    }
    return true;
}

template <class Book>
bool run_all(long steps, int seeds, std::uint64_t first_seed) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int s = 0; s < seeds; ++s) {
        if (!run_seed<Book>(first_seed + static_cast<std::uint64_t>(s), steps)) return false;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[pass]  %-36s %ld messages x %d seeds, identical to reference (%.1fs)\n", Book::kName,
                steps, seeds, secs);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const long steps = argc > 1 ? std::atol(argv[1]) : 200000;
    const int seeds = argc > 2 ? std::atoi(argv[2]) : 5;
    const std::uint64_t first_seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;

    bool ok = run_all<OrderBook>(steps, seeds, first_seed);
    ok = run_all<FastOrderBook>(steps, seeds, first_seed) && ok;
    return ok ? 0 : 1;
}
