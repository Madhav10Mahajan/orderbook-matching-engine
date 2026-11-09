// Replays an order CSV through the book, writes the trade tape, optionally samples
// the top of book after every message, and prints microstructure statistics.
//
// Usage:
//   replay <orders.csv> [--trades trades.csv] [--bbo bbo.csv] [--decimals 2] [--book v1|v2]
//
// Parsing happens before the timed section, so the reported throughput is the
// matching engine only (plus BBO sampling if --bbo is given).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "csv.hpp"
#include "fast_order_book.hpp"
#include "message.hpp"
#include "order_book.hpp"
#include "stats.hpp"

using namespace ob;

namespace {

struct Options {
    std::string input, trades_out, bbo_out, book = "v2";
    int decimals = 2;
};

struct TapeEntry {
    std::size_t msg_seq;
    Trade trade;
};

struct BboRow {
    std::size_t msg_seq;
    std::optional<Price> bid, ask;
    std::optional<double> imb1, imb5, micro;
    std::optional<Price> last_trade;
};

void usage() {
    std::fprintf(stderr,
                 "usage: replay <orders.csv> [--trades out.csv] [--bbo out.csv] [--decimals N] [--book v1|v2]\n");
}

std::string opt_price(std::optional<Price> p, int d) { return p ? format_price(*p, d) : ""; }
std::string opt_num(std::optional<double> v, double scale = 1.0) {
    if (!v) return "";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", *v / scale);
    return buf;
}

template <class Book>
int run(const Options& opt, const std::vector<Message>& msgs) {
    Book book;
    TradeStats stats;
    std::vector<TapeEntry> tape;
    std::vector<BboRow> bbo;
    std::size_t by_status[5] = {};
    const bool sample = !opt.bbo_out.empty();
    if (sample) bbo.reserve(msgs.size());

    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        ExecResult r = apply(book, msgs[i]);
        ++by_status[static_cast<int>(r.status)];
        for (const Trade& t : r.trades) {
            stats.add(t);
            tape.push_back({i + 1, t});
        }
        if (sample) {
            bbo.push_back({i + 1, book.best_bid(), book.best_ask(), imbalance(book, 1), imbalance(book, 5),
                           microprice(book), stats.count() ? std::optional<Price>(stats.last()) : std::nullopt});
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const int d = opt.decimals;
    double scale = 1.0;
    for (int i = 0; i < d; ++i) scale *= 10.0;

    std::printf("Replayed %zu messages with %s in %.3f ms (%.2f M msg/s)%s\n", msgs.size(), Book::kName,
                secs * 1e3, msgs.size() / secs / 1e6, sample ? " [incl. BBO sampling]" : "");
    std::printf("  accepted %zu | rejected: invalid qty %zu, duplicate id %zu, unknown id %zu, FOK killed %zu\n",
                by_status[0], by_status[1], by_status[2], by_status[3], by_status[4]);
    std::printf("Trades: %zu fills, volume %lld", stats.count(), static_cast<long long>(stats.volume()));
    if (auto v = stats.vwap()) {
        std::printf(", VWAP %.4f, high %s, low %s, last %s", *v / scale, format_price(stats.high(), d).c_str(),
                    format_price(stats.low(), d).c_str(), format_price(stats.last(), d).c_str());
    }
    std::printf("\nFinal book: %zu resting orders\n", book.order_count());
    std::printf("  best bid %s | best ask %s", opt_price(book.best_bid(), d).c_str(),
                opt_price(book.best_ask(), d).c_str());
    if (auto s = spread(book)) std::printf(" | spread %s", format_price(*s, d).c_str());
    if (auto m = mid_price(book)) std::printf(" | mid %.4f", *m / scale);
    if (auto mp = microprice(book)) std::printf(" | microprice %.4f", *mp / scale);
    std::printf("\n");
    if (auto i1 = imbalance(book, 1)) std::printf("  imbalance top-1 %+.3f", *i1);
    if (auto i5 = imbalance(book, 5)) std::printf(" | top-5 %+.3f", *i5);
    std::printf("\n  %-14s %10s   %-14s %10s\n", "bid", "qty", "ask", "qty");
    auto bids = book.depth(Side::Buy, 5), asks = book.depth(Side::Sell, 5);
    for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
        std::printf("  %-14s %10s   %-14s %10s\n", i < bids.size() ? format_price(bids[i].price, d).c_str() : "",
                    i < bids.size() ? std::to_string(bids[i].qty).c_str() : "",
                    i < asks.size() ? format_price(asks[i].price, d).c_str() : "",
                    i < asks.size() ? std::to_string(asks[i].qty).c_str() : "");
    }

    if (!opt.trades_out.empty()) {
        std::ofstream out(opt.trades_out);
        out << "trade_seq,msg_seq,buy_id,sell_id,price,qty\n";
        for (std::size_t i = 0; i < tape.size(); ++i) {
            const Trade& t = tape[i].trade;
            out << i + 1 << ',' << tape[i].msg_seq << ',' << t.buy_id << ',' << t.sell_id << ','
                << format_price(t.price, d) << ',' << t.qty << '\n';
        }
        std::printf("Wrote %zu trades to %s\n", tape.size(), opt.trades_out.c_str());
    }
    if (sample) {
        std::ofstream out(opt.bbo_out);
        out << "msg_seq,best_bid,best_ask,spread,mid,microprice,imbalance1,imbalance5,last_trade\n";
        for (const BboRow& b : bbo) {
            std::optional<Price> spr;
            std::optional<double> mid;
            if (b.bid && b.ask) {
                spr = *b.ask - *b.bid;
                mid = (static_cast<double>(*b.bid) + static_cast<double>(*b.ask)) / 2.0;
            }
            out << b.msg_seq << ',' << opt_price(b.bid, d) << ',' << opt_price(b.ask, d) << ','
                << opt_price(spr, d) << ',' << opt_num(mid, scale) << ',' << opt_num(b.micro, scale) << ','
                << opt_num(b.imb1) << ',' << opt_num(b.imb5) << ',' << opt_price(b.last_trade, d) << '\n';
        }
        std::printf("Wrote %zu BBO snapshots to %s\n", bbo.size(), opt.bbo_out.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--trades")) opt.trades_out = next();
        else if (!std::strcmp(argv[i], "--bbo")) opt.bbo_out = next();
        else if (!std::strcmp(argv[i], "--decimals")) opt.decimals = std::atoi(next());
        else if (!std::strcmp(argv[i], "--book")) opt.book = next();
        else if (argv[i][0] == '-') { usage(); return 2; }
        else opt.input = argv[i];
    }
    if (opt.input.empty() || (opt.book != "v1" && opt.book != "v2")) {
        usage();
        return 2;
    }

    std::ifstream in(opt.input);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", opt.input.c_str());
        return 1;
    }
    std::vector<Message> msgs;
    std::string line, error;
    std::size_t line_no = 0, bad = 0;
    while (std::getline(in, line)) {
        ++line_no;
        Message m;
        switch (parse_line(line, opt.decimals, m, error)) {
            case ParseResult::Message: msgs.push_back(m); break;
            case ParseResult::Skip: break;
            case ParseResult::Error:
                if (++bad <= 10) std::fprintf(stderr, "line %zu: %s (skipped)\n", line_no, error.c_str());
                break;
        }
    }
    if (bad) std::fprintf(stderr, "%zu malformed lines skipped\n", bad);

    return opt.book == "v1" ? run<OrderBook>(opt, msgs) : run<FastOrderBook>(opt, msgs);
}
