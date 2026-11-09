// Benchmark: throughput and per-message latency of v1 vs v2 on the same synthetic
// order flow (include/order_flow.hpp).
//
// Usage: bench [messages=2000000] [seed=42] [--csv results.csv]
//
// Method:
//   1. Generate every message up front, so generation cost is never timed.
//   2. Throughput: process the whole stream on a fresh book, best of 3 runs.
//   3. Latency: process it again, timing each message individually, and report
//      percentiles per message type. Tails (p99, p99.9) matter more than the mean
//      in trading, because the slow messages are the ones that cost money.
//   4. Report the clock's own overhead and resolution: on Apple Silicon the
//      timer ticks every ~41.7 ns, so single-message latencies are quantised to
//      that step. Throughput (total time / messages) has no such limit.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "fast_order_book.hpp"
#include "message.hpp"
#include "order_book.hpp"
#include "order_flow.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

struct Percentiles {
    std::size_t n = 0;
    double mean = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

Percentiles summarize(std::vector<std::uint32_t>& v) {
    Percentiles p;
    p.n = v.size();
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return static_cast<double>(v[std::min(v.size() - 1, static_cast<std::size_t>(q * v.size()))]); };
    double sum = 0;
    for (auto x : v) sum += x;
    p.mean = sum / v.size();
    p.p50 = at(0.50);
    p.p90 = at(0.90);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    p.max = v.back();
    return p;
}

struct BookResult {
    const char* name;
    double best_secs = 0;
    Percentiles all, add, cancel, modify, market;
    std::size_t trades = 0, final_orders = 0;
};

template <class Book>
BookResult run(const std::vector<Message>& msgs) {
    BookResult res;
    res.name = Book::kName;

    // Throughput: best of 3 full runs on a fresh book.
    res.best_secs = 1e9;
    for (int rep = 0; rep < 3; ++rep) {
        Book book;
        std::size_t trades = 0;
        const auto t0 = Clock::now();
        for (const Message& m : msgs) trades += apply(book, m).trades.size();
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        res.best_secs = std::min(res.best_secs, secs);
        res.trades = trades;
        res.final_orders = book.order_count();
    }

    // Latency: time every message.
    std::vector<std::uint32_t> all, add, cancel, modify, market;
    all.reserve(msgs.size());
    Book book;
    std::size_t sink = 0;
    for (const Message& m : msgs) {
        const auto t0 = Clock::now();
        sink += apply(book, m).trades.size();
        const auto t1 = Clock::now();
        const auto ns = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        all.push_back(ns);
        switch (m.type) {
            case MsgType::Add: add.push_back(ns); break;
            case MsgType::Cancel: cancel.push_back(ns); break;
            case MsgType::Modify: modify.push_back(ns); break;
            case MsgType::Market: market.push_back(ns); break;
        }
    }
    if (sink != res.trades) std::printf("warning: non-deterministic trade count\n");
    res.all = summarize(all);
    res.add = summarize(add);
    res.cancel = summarize(cancel);
    res.modify = summarize(modify);
    res.market = summarize(market);
    return res;
}

void print_row(const char* label, const Percentiles& p) {
    std::printf("  %-8s %9zu %8.0f %7.0f %7.0f %7.0f %8.0f %9.0f\n", label, p.n, p.mean, p.p50, p.p90, p.p99,
                p.p999, p.max);
}

}  // namespace

int main(int argc, char** argv) {
    long n = 2000000;
    std::uint64_t seed = 42;
    std::string csv;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (positional++ == 0) n = std::atol(argv[i]);
        else seed = std::strtoull(argv[i], nullptr, 10);
    }

    std::vector<Message> msgs;
    msgs.reserve(static_cast<std::size_t>(n));
    OrderFlowGenerator gen(seed);
    for (long i = 0; i < n; ++i) msgs.push_back(gen.next());
    std::size_t counts[4] = {};
    for (const auto& m : msgs) {
        counts[m.type == MsgType::Add ? 0 : m.type == MsgType::Cancel ? 1 : m.type == MsgType::Modify ? 2 : 3]++;
    }

    // Clock overhead (median back-to-back now() delta) and resolution (smallest non-zero step).
    std::vector<std::uint32_t> deltas;
    std::int64_t resolution = INT64_MAX;
    for (int i = 0; i < 200000; ++i) {
        const auto a = Clock::now(), b = Clock::now();
        const auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        deltas.push_back(static_cast<std::uint32_t>(d));
        if (d > 0) resolution = std::min<std::int64_t>(resolution, d);
    }
    std::sort(deltas.begin(), deltas.end());

    std::printf("Workload: %ld messages (seed %llu): %.1f%% add, %.1f%% cancel, %.1f%% modify, %.1f%% market\n", n,
                static_cast<unsigned long long>(seed), 100.0 * counts[0] / n, 100.0 * counts[1] / n,
                100.0 * counts[2] / n, 100.0 * counts[3] / n);
    std::printf("Clock: steady_clock, median overhead %u ns, resolution %lld ns\n\n", deltas[deltas.size() / 2],
                static_cast<long long>(resolution));

    BookResult results[] = {run<OrderBook>(msgs), run<FastOrderBook>(msgs)};

    for (const auto& r : results) {
        std::printf("%s\n", r.name);
        std::printf("  throughput: %.2f M msg/s (%.1f ns/msg avg), %zu fills, %zu orders resting at end\n",
                    n / r.best_secs / 1e6, r.best_secs * 1e9 / n, r.trades, r.final_orders);
        std::printf("  latency ns  %9s %8s %7s %7s %7s %8s %9s\n", "count", "mean", "p50", "p90", "p99", "p99.9",
                    "max");
        print_row("all", r.all);
        print_row("add", r.add);
        print_row("cancel", r.cancel);
        print_row("modify", r.modify);
        print_row("market", r.market);
        std::printf("\n");
    }
    const auto& v1 = results[0];
    const auto& v2 = results[1];
    std::printf("v2 vs v1: throughput %.2fx, p50 %.2fx, p99 %.2fx, p99.9 %.2fx faster\n", v1.best_secs / v2.best_secs,
                v1.all.p50 / std::max(1.0, v2.all.p50), v1.all.p99 / std::max(1.0, v2.all.p99),
                v1.all.p999 / std::max(1.0, v2.all.p999));

    if (!csv.empty()) {
        std::ofstream out(csv);
        out << "book,msgs,msgs_per_sec,ns_per_msg,type,count,mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,max_ns\n";
        for (const auto& r : results) {
            const std::pair<const char*, const Percentiles*> rows[] = {
                {"all", &r.all}, {"add", &r.add}, {"cancel", &r.cancel}, {"modify", &r.modify}, {"market", &r.market}};
            for (auto [label, p] : rows) {
                out << '"' << r.name << "\"," << n << ',' << n / r.best_secs << ',' << r.best_secs * 1e9 / n << ','
                    << label << ',' << p->n << ',' << p->mean << ',' << p->p50 << ',' << p->p90 << ',' << p->p99
                    << ',' << p->p999 << ',' << p->max << '\n';
            }
        }
        std::printf("Wrote %s\n", csv.c_str());
    }
    return 0;
}
