// Demo: replays the worked example from the walkthrough and prints the book after each step.
#include <cstdio>
#include <string>

#include "order_book.hpp"

using namespace ob;

// Prices are integer ticks of $0.01; convert only for display.
static std::string dollars(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "$%lld.%02lld", static_cast<long long>(p / 100),
                  static_cast<long long>((p < 0 ? -p : p) % 100));
    return buf;
}

static void print_book(const OrderBook& book) {
    auto asks = book.depth(Side::Sell, 5);
    auto bids = book.depth(Side::Buy, 5);
    std::printf("    ASKS\n");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {  // highest ask on top
        std::printf("      %-9s %5lld sh  (%zu orders)\n", dollars(it->price).c_str(),
                    static_cast<long long>(it->qty), it->order_count);
    }
    std::printf("    ------------------------------\n");
    for (const auto& lvl : bids) {
        std::printf("      %-9s %5lld sh  (%zu orders)\n", dollars(lvl.price).c_str(),
                    static_cast<long long>(lvl.qty), lvl.order_count);
    }
    std::printf("    BIDS\n\n");
}

static void report(const char* what, const ExecResult& r) {
    std::printf("> %s\n", what);
    for (const Trade& t : r.trades) {
        std::printf("    TRADE buyer #%llu  seller #%llu  %d sh @ %s\n",
                    static_cast<unsigned long long>(t.buy_id),
                    static_cast<unsigned long long>(t.sell_id), t.qty, dollars(t.price).c_str());
    }
    if (r.trades.empty()) std::printf("    (no trades)\n");
    if (r.rested > 0) std::printf("    %d sh rest in the book\n", r.rested);
}

int main() {
    OrderBook book;
    book.add_limit(4, Side::Sell, 50200, 10);
    book.add_limit(1, Side::Sell, 50100, 5);
    book.add_limit(2, Side::Sell, 50100, 3);
    book.add_limit(3, Side::Buy, 49900, 4);
    book.add_limit(5, Side::Buy, 49800, 10);
    book.add_limit(6, Side::Buy, 49800, 5);
    std::printf("Starting book:\n");
    print_book(book);

    report("ADD #8 BUY 6 @ $500 (no match)", book.add_limit(8, Side::Buy, 50000, 6));
    print_book(book);

    std::printf("> CANCEL #8 -> %s\n", book.cancel(8) == Status::Ok ? "ok" : "unknown");
    print_book(book);

    report("ADD #10 BUY 12 @ $501 (partial match, rest)", book.add_limit(10, Side::Buy, 50100, 12));
    print_book(book);

    report("MARKET #11 SELL 7", book.add_market(11, Side::Sell, 7));
    print_book(book);

    return 0;
}
