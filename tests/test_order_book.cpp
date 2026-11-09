// Unit tests. Every order-book scenario is a template and runs against BOTH
// OrderBook (v1) and FastOrderBook (v2). Scenarios follow the worked examples
// from docs/01_PROBLEM_STATEMENT.md. Prices are in cents: 50100 = $501.00.
//
// Tiny self-contained harness: CHECK records a failure and keeps going, so one run
// reports every broken expectation. Built with ASan/UBSan (see Makefile).
#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "csv.hpp"
#include "fast_order_book.hpp"
#include "flat_id_map.hpp"
#include "message.hpp"
#include "order_book.hpp"
#include "stats.hpp"

using namespace ob;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

// Every test calls this after each operation: the book must always be consistent.
#define CHECK_INVARIANTS(book)                                                   \
    do {                                                                         \
        ++g_checks;                                                              \
        std::string err = (book).check_invariants();                             \
        if (!err.empty()) {                                                      \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d  invariant: %s\n", __FILE__, __LINE__, err.c_str()); \
        }                                                                        \
    } while (0)

// The starting book from the walkthrough:
//   ASKS  $502 -> [#4:10]
//         $501 -> [#1:5, #2:3]
//   BIDS  $499 -> [#3:4]
//         $498 -> [#5:10, #6:5]
template <class Book>
static void fill_example(Book& book) {
    book.add_limit(4, Side::Sell, 50200, 10);
    book.add_limit(1, Side::Sell, 50100, 5);
    book.add_limit(2, Side::Sell, 50100, 3);
    book.add_limit(3, Side::Buy, 49900, 4);
    book.add_limit(5, Side::Buy, 49800, 10);
    book.add_limit(6, Side::Buy, 49800, 5);
}

// ===========================================================================
// Core matching (phases 3-4)
// ===========================================================================

template <class Book>
static void test_empty_book() {
    Book book;
    CHECK(!book.best_bid().has_value());
    CHECK(!book.best_ask().has_value());
    CHECK(book.order_count() == 0);
    CHECK(book.depth(Side::Buy, 5).empty());
    CHECK(book.cancel(42) == Status::UnknownId);
    CHECK(!book.order_qty(42).has_value());
    CHECK_INVARIANTS(book);
}

template <class Book>
static void test_example_book_setup() {
    Book book;
    fill_example(book);
    CHECK(book.best_bid() == 49900);
    CHECK(book.best_ask() == 50100);
    CHECK(book.order_count() == 6);
    CHECK(book.volume_at(Side::Sell, 50100) == 8);
    CHECK(book.volume_at(Side::Buy, 49800) == 15);
    CHECK(book.volume_at(Side::Buy, 12345) == 0);
    CHECK(book.order_qty(2) == 3);

    auto asks = book.depth(Side::Sell, 10);
    CHECK(asks.size() == 2);
    CHECK(asks[0].price == 50100 && asks[0].qty == 8 && asks[0].order_count == 2);  // best first
    CHECK(asks[1].price == 50200 && asks[1].qty == 10);

    auto bids = book.depth(Side::Buy, 1);  // n limits the levels returned
    CHECK(bids.size() == 1);
    CHECK(bids[0].price == 49900);
    CHECK_INVARIANTS(book);
}

// Case 1: BUY 6 @ $500 — cheapest seller is $501, nothing trades, order rests.
template <class Book>
static void test_no_match_rests_new_level() {
    Book book;
    fill_example(book);
    auto r = book.add_limit(8, Side::Buy, 50000, 6);
    CHECK(r.status == Status::Ok);
    CHECK(r.trades.empty());
    CHECK(r.rested == 6);
    CHECK(book.best_bid() == 50000);
    CHECK(book.best_ask() == 50100);
    CHECK_INVARIANTS(book);
}

// Case 1 variant: BUY 6 @ $499 joins the BACK of the $499 queue, behind #3.
template <class Book>
static void test_joins_back_of_existing_level() {
    Book book;
    fill_example(book);
    book.add_limit(8, Side::Buy, 49900, 6);
    auto bids = book.depth(Side::Buy, 1);
    CHECK(bids[0].price == 49900 && bids[0].qty == 10 && bids[0].order_count == 2);

    auto r = book.add_limit(20, Side::Sell, 49900, 5);
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0] == (Trade{3, 20, 49900, 4}));
    CHECK(r.trades[1] == (Trade{8, 20, 49900, 1}));
    CHECK(book.volume_at(Side::Buy, 49900) == 5);
    CHECK_INVARIANTS(book);
}

// Case 2: BUY 7 @ $501 — fills #1 fully (5), #2 partially (2). Nothing rests.
template <class Book>
static void test_full_match() {
    Book book;
    fill_example(book);
    auto r = book.add_limit(9, Side::Buy, 50100, 7);
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0] == (Trade{9, 1, 50100, 5}));
    CHECK(r.trades[1] == (Trade{9, 2, 50100, 2}));
    CHECK(r.rested == 0);
    CHECK(book.volume_at(Side::Sell, 50100) == 1);
    CHECK(book.order_qty(2) == 1);
    CHECK(book.best_bid() == 49900);
    CHECK(book.cancel(1) == Status::UnknownId);
    CHECK(book.cancel(9) == Status::UnknownId);
    CHECK_INVARIANTS(book);
}

// Case 3: BUY 12 @ $501 — takes all 8 at $501, can't reach $502, 4 rest as new best bid.
template <class Book>
static void test_partial_match_then_rest() {
    Book book;
    fill_example(book);
    auto r = book.add_limit(10, Side::Buy, 50100, 12);
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0] == (Trade{10, 1, 50100, 5}));
    CHECK(r.trades[1] == (Trade{10, 2, 50100, 3}));
    CHECK(r.rested == 4);
    CHECK(book.best_ask() == 50200);
    CHECK(book.best_bid() == 50100);
    CHECK(book.volume_at(Side::Buy, 50100) == 4);
    CHECK_INVARIANTS(book);
}

// Case 4: BUY 12 @ $505 — sweeps two levels, pays the RESTING prices ($501, $502), not $505.
template <class Book>
static void test_sweep_multiple_levels() {
    Book book;
    fill_example(book);
    auto r = book.add_limit(11, Side::Buy, 50500, 12);
    CHECK(r.trades.size() == 3);
    CHECK(r.trades[0] == (Trade{11, 1, 50100, 5}));
    CHECK(r.trades[1] == (Trade{11, 2, 50100, 3}));
    CHECK(r.trades[2] == (Trade{11, 4, 50200, 4}));
    CHECK(r.rested == 0);
    CHECK(book.best_ask() == 50200);
    CHECK(book.volume_at(Side::Sell, 50200) == 6);
    CHECK_INVARIANTS(book);
}

template <class Book>
static void test_sell_side_mirror() {
    Book book;
    fill_example(book);
    auto r = book.add_limit(12, Side::Sell, 49900, 5);
    CHECK(r.trades.size() == 1);
    CHECK(r.trades[0] == (Trade{3, 12, 49900, 4}));
    CHECK(r.rested == 1);
    CHECK(book.best_ask() == 49900);
    CHECK(book.best_bid() == 49800);
    CHECK_INVARIANTS(book);
}

// The t1..t8 cancel timeline from the walkthrough.
template <class Book>
static void test_cancel_timeline() {
    Book book;
    book.add_limit(1, Side::Sell, 50100, 5);
    book.add_limit(2, Side::Sell, 50100, 3);
    book.add_limit(7, Side::Sell, 50100, 2);
    book.add_limit(4, Side::Sell, 50200, 10);
    CHECK_INVARIANTS(book);

    CHECK(book.cancel(2) == Status::Ok);  // t5: from the middle
    CHECK(book.volume_at(Side::Sell, 50100) == 7);
    CHECK(book.depth(Side::Sell, 1)[0].order_count == 2);
    CHECK_INVARIANTS(book);

    auto r = book.add_limit(10, Side::Buy, 50100, 6);  // t6
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0] == (Trade{10, 1, 50100, 5}));
    CHECK(r.trades[1] == (Trade{10, 7, 50100, 1}));
    CHECK_INVARIANTS(book);

    CHECK(book.cancel(7) == Status::Ok);  // t7: level empties
    CHECK(book.best_ask() == 50200);
    CHECK(book.volume_at(Side::Sell, 50100) == 0);
    CHECK_INVARIANTS(book);

    CHECK(book.cancel(1) == Status::UnknownId);  // t8
    CHECK(book.cancel(7) == Status::UnknownId);
    CHECK(book.order_count() == 1);
    CHECK_INVARIANTS(book);
}

template <class Book>
static void test_cancel_bid_and_reuse_id() {
    Book book;
    fill_example(book);
    CHECK(book.cancel(5) == Status::Ok);
    CHECK(book.volume_at(Side::Buy, 49800) == 5);
    CHECK_INVARIANTS(book);

    CHECK(book.add_limit(5, Side::Buy, 49800, 1).status == Status::Ok);
    auto r = book.add_limit(30, Side::Sell, 49800, 100);
    CHECK(r.trades.size() == 3);
    CHECK(r.trades[0].buy_id == 3);
    CHECK(r.trades[1].buy_id == 6);
    CHECK(r.trades[2].buy_id == 5);
    CHECK(r.rested == 100 - 4 - 5 - 1);
    CHECK(!book.best_bid().has_value());
    CHECK_INVARIANTS(book);
}

template <class Book>
static void test_market_orders() {
    Book book;
    fill_example(book);
    auto r = book.add_market(40, Side::Buy, 20);
    CHECK(r.status == Status::Ok);
    CHECK(r.trades.size() == 3);
    CHECK(r.trades[2] == (Trade{40, 4, 50200, 10}));
    CHECK(r.rested == 0);
    CHECK(!book.best_ask().has_value());
    CHECK(book.best_bid() == 49900);
    CHECK_INVARIANTS(book);

    auto r2 = book.add_market(41, Side::Buy, 5);
    CHECK(r2.status == Status::Ok && r2.trades.empty());

    auto r3 = book.add_market(42, Side::Sell, 6);
    CHECK(r3.trades.size() == 2);
    CHECK(r3.trades[0] == (Trade{3, 42, 49900, 4}));
    CHECK(r3.trades[1] == (Trade{5, 42, 49800, 2}));
    CHECK_INVARIANTS(book);
}

template <class Book>
static void test_rejects_leave_book_unchanged() {
    Book book;
    fill_example(book);
    CHECK(book.add_limit(50, Side::Buy, 50100, 0).status == Status::InvalidQuantity);
    CHECK(book.add_limit(51, Side::Buy, 50100, -5).status == Status::InvalidQuantity);
    CHECK(book.add_market(52, Side::Buy, 0).status == Status::InvalidQuantity);
    auto dup = book.add_limit(1, Side::Buy, 50500, 100);
    CHECK(dup.status == Status::DuplicateId);
    CHECK(dup.trades.empty());
    CHECK(book.add_market(2, Side::Sell, 1).status == Status::DuplicateId);

    CHECK(book.order_count() == 6);
    CHECK(book.best_ask() == 50100 && book.best_bid() == 49900);
    CHECK_INVARIANTS(book);
}

template <class Book>
static void test_negative_prices() {
    Book book;  // e.g. WTI crude, April 2020
    book.add_limit(1, Side::Sell, -3763, 10);
    book.add_limit(2, Side::Sell, -3700, 10);
    auto r = book.add_limit(3, Side::Buy, -3750, 15);
    CHECK(r.trades.size() == 1);
    CHECK(r.trades[0] == (Trade{3, 1, -3763, 10}));
    CHECK(r.rested == 5);
    CHECK(book.best_bid() == -3750 && book.best_ask() == -3700);
    CHECK_INVARIANTS(book);
}

// ===========================================================================
// Time-in-force (phase 6)
// ===========================================================================

template <class Book>
static void test_ioc() {
    Book book;
    fill_example(book);
    // IOC BUY 12 @ $501: fills 8 at $501, the other 4 are discarded instead of resting.
    auto r = book.add_limit(60, Side::Buy, 50100, 12, TimeInForce::IOC);
    CHECK(r.status == Status::Ok);
    CHECK(r.trades.size() == 2);
    CHECK(r.rested == 0);
    CHECK(book.best_bid() == 49900);  // nothing of #60 rested
    CHECK(!book.order_qty(60).has_value());
    CHECK_INVARIANTS(book);

    // IOC that crosses nothing: no trades, no effect.
    auto r2 = book.add_limit(61, Side::Buy, 50000, 5, TimeInForce::IOC);
    CHECK(r2.status == Status::Ok && r2.trades.empty() && r2.rested == 0);
    CHECK(book.order_count() == 4);
    CHECK_INVARIANTS(book);
}

template <class Book>
static void test_fok() {
    Book book;
    fill_example(book);
    // 18 shares are offered at <= $502. FOK for 19 must be killed with NO trades.
    auto killed = book.add_limit(70, Side::Buy, 50200, 19, TimeInForce::FOK);
    CHECK(killed.status == Status::Killed);
    CHECK(killed.trades.empty());
    CHECK(book.volume_at(Side::Sell, 50100) == 8 && book.volume_at(Side::Sell, 50200) == 10);
    CHECK_INVARIANTS(book);

    // Limit too low to reach $502: only 8 available -> killed.
    CHECK(book.add_limit(71, Side::Buy, 50100, 9, TimeInForce::FOK).status == Status::Killed);

    // Exactly 18 available -> fills completely across both levels.
    auto filled = book.add_limit(72, Side::Buy, 50200, 18, TimeInForce::FOK);
    CHECK(filled.status == Status::Ok);
    CHECK(filled.trades.size() == 3);
    CHECK(filled.rested == 0);
    CHECK(!book.best_ask().has_value());
    CHECK_INVARIANTS(book);

    // Sell-side FOK.
    auto sell = book.add_limit(73, Side::Sell, 49800, 19, TimeInForce::FOK);
    CHECK(sell.status == Status::Ok && sell.trades.size() == 3);
    CHECK(!book.best_bid().has_value());
    CHECK_INVARIANTS(book);
}

// ===========================================================================
// Modify (phase 6)
// ===========================================================================

template <class Book>
static void test_modify_reduce_keeps_priority() {
    Book book;
    fill_example(book);
    // #1 (front of $501) reduces 5 -> 2: stays at the front.
    auto r = book.modify(1, 50100, 2);
    CHECK(r.status == Status::Ok && r.trades.empty() && r.rested == 2);
    CHECK(book.volume_at(Side::Sell, 50100) == 5);
    CHECK_INVARIANTS(book);

    auto hit = book.add_market(80, Side::Buy, 1);
    CHECK(hit.trades.size() == 1 && hit.trades[0].sell_id == 1);  // #1 still first in line
    CHECK_INVARIANTS(book);

    // Same price, same qty: no-op, still Ok.
    CHECK(book.modify(1, 50100, 1).status == Status::Ok);
    CHECK(book.order_qty(1) == 1);
}

template <class Book>
static void test_modify_increase_loses_priority() {
    Book book;
    fill_example(book);
    // #1 increases 5 -> 6 at the same price: goes BEHIND #2.
    auto r = book.modify(1, 50100, 6);
    CHECK(r.status == Status::Ok && r.rested == 6);
    CHECK(book.volume_at(Side::Sell, 50100) == 9);
    CHECK_INVARIANTS(book);

    auto hit = book.add_market(81, Side::Buy, 4);
    CHECK(hit.trades.size() == 2);
    CHECK(hit.trades[0] == (Trade{81, 2, 50100, 3}));  // #2 now first
    CHECK(hit.trades[1] == (Trade{81, 1, 50100, 1}));
    CHECK_INVARIANTS(book);
}

template <class Book>
static void test_modify_price_moves_and_can_trade() {
    Book book;
    fill_example(book);
    // Move #4 from $502 down to $501: joins the back of the $501 queue.
    CHECK(book.modify(4, 50100, 10).status == Status::Ok);
    CHECK(book.volume_at(Side::Sell, 50200) == 0);
    CHECK(book.volume_at(Side::Sell, 50100) == 18);
    CHECK(book.depth(Side::Sell, 1)[0].order_count == 3);
    CHECK_INVARIANTS(book);

    // Move bid #3 up to $501: crosses and trades against the asks (#1 first).
    auto r = book.modify(3, 50100, 4);
    CHECK(r.trades.size() == 1);
    CHECK(r.trades[0] == (Trade{3, 1, 50100, 4}));
    CHECK(r.rested == 0);
    CHECK(!book.order_qty(3).has_value());
    CHECK(book.best_bid() == 49800);
    CHECK_INVARIANTS(book);
}

template <class Book>
static void test_modify_rejects() {
    Book book;
    fill_example(book);
    CHECK(book.modify(999, 50100, 5).status == Status::UnknownId);
    CHECK(book.modify(1, 50100, 0).status == Status::InvalidQuantity);
    CHECK(book.modify(1, 50100, -1).status == Status::InvalidQuantity);
    CHECK(book.order_qty(1) == 5);
    CHECK(book.order_count() == 6);
    CHECK_INVARIANTS(book);
}

// ===========================================================================
// Random flow + statistics
// ===========================================================================

template <class Book>
static void test_random_flow_keeps_invariants() {
    Book book;
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<int> action(0, 11), qty(1, 50), price(9990, 10010);
    std::vector<OrderId> live;
    OrderId next_id = 1;

    int bad_steps = 0;
    for (int step = 0; step < 20000; ++step) {
        const int a = action(rng);
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        if (a < 6) {
            OrderId id = next_id++;
            const TimeInForce tif = a == 0 ? TimeInForce::IOC : (a == 1 ? TimeInForce::FOK : TimeInForce::GTC);
            auto r = book.add_limit(id, side, price(rng), qty(rng), tif);
            if (r.rested > 0) live.push_back(id);
        } else if (a < 9 && !live.empty()) {
            std::size_t i = rng() % live.size();
            book.cancel(live[i]);
            live[i] = live.back();
            live.pop_back();
        } else if (a < 11 && !live.empty()) {
            book.modify(live[rng() % live.size()], price(rng), qty(rng));
        } else {
            book.add_market(next_id++, side, qty(rng));
        }
        if (!book.check_invariants().empty()) ++bad_steps;
    }
    CHECK(bad_steps == 0);
    CHECK_INVARIANTS(book);
}

template <class Book>
static void test_stats() {
    Book book;
    CHECK(!spread(book).has_value() && !mid_price(book).has_value());
    CHECK(!imbalance(book, 1).has_value() && !microprice(book).has_value());

    fill_example(book);
    CHECK(spread(book) == 200);           // 50100 - 49900
    CHECK(mid_price(book) == 50000.0);
    // top-1: bid 4 vs ask 8 -> (4 - 8) / 12
    CHECK(*imbalance(book, 1) == -4.0 / 12.0);
    // top-5: bids 4+15=19, asks 8+10=18 -> 1/37
    CHECK(*imbalance(book, 5) == 1.0 / 37.0);
    // microprice = (49900*8 + 50100*4) / 12, pulled toward the thin side (bid)
    CHECK(*microprice(book) == (49900.0 * 8 + 50100.0 * 4) / 12.0);

    TradeStats ts;
    CHECK(!ts.vwap().has_value());
    for (const Trade& t : book.add_limit(90, Side::Buy, 50200, 12).trades) ts.add(t);
    // 5 @ 50100 + 3 @ 50100 + 4 @ 50200
    CHECK(ts.count() == 3 && ts.volume() == 12);
    CHECK(*ts.vwap() == (50100.0 * 8 + 50200.0 * 4) / 12.0);
    CHECK(ts.high() == 50200 && ts.low() == 50100 && ts.last() == 50200);
}

// ===========================================================================
// Non-book components
// ===========================================================================

static void test_parse_price() {
    CHECK(parse_price("101.37", 2) == 10137);
    CHECK(parse_price("101", 2) == 10100);
    CHECK(parse_price("101.5", 2) == 10150);
    CHECK(parse_price("101.500", 2) == 10150);  // trailing zeros are on-grid
    CHECK(parse_price(".05", 2) == 5);
    CHECK(parse_price("-37.63", 2) == -3763);
    CHECK(parse_price("+1.00", 2) == 100);
    CHECK(parse_price(" 7.25 ", 2) == 725);
    CHECK(parse_price("100.0001", 4) == 1000001);
    CHECK(!parse_price("101.375", 2).has_value());  // off the tick grid
    CHECK(!parse_price("abc", 2).has_value());
    CHECK(!parse_price("", 2).has_value());
    CHECK(!parse_price("1.2.3", 2).has_value());
    CHECK(!parse_price("--5", 2).has_value());
    CHECK(!parse_price("1e5", 2).has_value());
    // The point of integer ticks: 0.1 + 0.2 == 0.3 exactly.
    CHECK(*parse_price("0.10", 2) + *parse_price("0.20", 2) == *parse_price("0.30", 2));

    CHECK(format_price(10137, 2) == "101.37");
    CHECK(format_price(5, 2) == "0.05");
    CHECK(format_price(-3763, 2) == "-37.63");
    CHECK(format_price(-5, 2) == "-0.05");
    CHECK(format_price(42, 0) == "42");
}

static void test_parse_line() {
    Message m;
    std::string err;
    CHECK(parse_line("A,1,S,101.00,5,GTC", 2, m, err) == ParseResult::Message);
    CHECK(m.type == MsgType::Add && m.id == 1 && m.side == Side::Sell && m.price == 10100 && m.qty == 5 &&
          m.tif == TimeInForce::GTC);
    CHECK(parse_line("A,2,B,99.5,10,IOC   # comment", 2, m, err) == ParseResult::Message);
    CHECK(m.side == Side::Buy && m.price == 9950 && m.tif == TimeInForce::IOC);
    CHECK(parse_line("A,3,B,99.50,10,FOK", 2, m, err) == ParseResult::Message && m.tif == TimeInForce::FOK);
    CHECK(parse_line("A,4,B,99.50,10", 2, m, err) == ParseResult::Message && m.tif == TimeInForce::GTC);
    CHECK(parse_line("M,3,B,,4,", 2, m, err) == ParseResult::Message);
    CHECK(m.type == MsgType::Market && m.qty == 4);
    CHECK(parse_line("C,1,,,,", 2, m, err) == ParseResult::Message && m.type == MsgType::Cancel && m.id == 1);
    CHECK(parse_line("X,2,,101.50,2,", 2, m, err) == ParseResult::Message);
    CHECK(m.type == MsgType::Modify && m.price == 10150 && m.qty == 2);

    CHECK(parse_line("", 2, m, err) == ParseResult::Skip);
    CHECK(parse_line("   # only a comment", 2, m, err) == ParseResult::Skip);
    CHECK(parse_line("type,id,side,price,qty,tif", 2, m, err) == ParseResult::Skip);
    CHECK(parse_line("A,1,Q,101.00,5,GTC", 2, m, err) == ParseResult::Error);
    CHECK(parse_line("A,1,S,101.001,5,GTC", 2, m, err) == ParseResult::Error);
    CHECK(parse_line("A,x,S,101.00,5,GTC", 2, m, err) == ParseResult::Error);
    CHECK(parse_line("A,1,S,101.00,5,DAY", 2, m, err) == ParseResult::Error);
    CHECK(parse_line("Z,1,S,101.00,5,GTC", 2, m, err) == ParseResult::Error);

    // Round trip: to_csv then parse gives the same message.
    Message in{MsgType::Add, 77, Side::Sell, -3763, 12, TimeInForce::FOK};
    Message out;
    CHECK(parse_line(to_csv(in, 2), 2, out, err) == ParseResult::Message);
    CHECK(out.type == in.type && out.id == in.id && out.side == in.side && out.price == in.price &&
          out.qty == in.qty && out.tif == in.tif);
}

static void test_apply_dispatch() {
    FastOrderBook book;
    CHECK(apply(book, Message{MsgType::Add, 1, Side::Sell, 100, 5, TimeInForce::GTC}).rested == 5);
    CHECK(apply(book, Message{MsgType::Modify, 1, Side::Sell, 100, 3, TimeInForce::GTC}).rested == 3);
    CHECK(apply(book, Message{MsgType::Market, 2, Side::Buy, 0, 1, TimeInForce::GTC}).trades.size() == 1);
    CHECK(apply(book, Message{MsgType::Cancel, 1, Side::Buy, 0, 0, TimeInForce::GTC}).status == Status::Ok);
    CHECK(apply(book, Message{MsgType::Cancel, 1, Side::Buy, 0, 0, TimeInForce::GTC}).status == Status::UnknownId);
}

// Random inserts/erases checked against std::unordered_map, including table growth
// and long probe runs (backward-shift deletion is the subtle part).
static void test_flat_id_map() {
    FlatIdMap map(16);
    std::unordered_map<OrderId, std::uint32_t> truth;
    std::mt19937_64 rng(7);
    int mismatches = 0;
    for (int step = 0; step < 200000; ++step) {
        // Small key space -> lots of collisions and re-inserts of erased keys.
        const OrderId key = rng() % 5000;
        if (rng() % 3 == 0) {
            const bool erased = map.erase(key);
            if (erased != (truth.erase(key) == 1)) ++mismatches;
        } else if (!truth.count(key)) {
            const auto v = static_cast<std::uint32_t>(rng() % 1000000);
            map.insert(key, v);
            truth[key] = v;
        }
        if (step % 1000 == 0) {
            for (OrderId k = 0; k < 5000; ++k) {
                auto it = truth.find(k);
                const std::uint32_t expect = it == truth.end() ? FlatIdMap::kNotFound : it->second;
                if (map.find(k) != expect) ++mismatches;
            }
            if (map.size() != truth.size()) ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    std::size_t seen = 0;
    map.for_each([&](OrderId k, std::uint32_t v) { seen += truth.count(k) && truth[k] == v; });
    CHECK(seen == truth.size());
}

// ===========================================================================

struct TestCase {
    std::string name;
    void (*fn)();
};

template <class Book>
static void add_book_tests(std::vector<TestCase>& tests, const std::string& tag) {
    const TestCase cases[] = {
        {"empty book", test_empty_book<Book>},
        {"example book setup", test_example_book_setup<Book>},
        {"case 1: no match, new level", test_no_match_rests_new_level<Book>},
        {"case 1b: joins back of queue (FIFO)", test_joins_back_of_existing_level<Book>},
        {"case 2: full match", test_full_match<Book>},
        {"case 3: partial match then rest", test_partial_match_then_rest<Book>},
        {"case 4: sweep multiple levels", test_sweep_multiple_levels<Book>},
        {"sell-side mirror", test_sell_side_mirror<Book>},
        {"cancel timeline t1-t8", test_cancel_timeline<Book>},
        {"cancel bid + reuse id", test_cancel_bid_and_reuse_id<Book>},
        {"market orders", test_market_orders<Book>},
        {"rejects leave book unchanged", test_rejects_leave_book_unchanged<Book>},
        {"negative prices", test_negative_prices<Book>},
        {"IOC", test_ioc<Book>},
        {"FOK", test_fok<Book>},
        {"modify: reduce keeps priority", test_modify_reduce_keeps_priority<Book>},
        {"modify: increase loses priority", test_modify_increase_loses_priority<Book>},
        {"modify: price change moves/trades", test_modify_price_moves_and_can_trade<Book>},
        {"modify: rejects", test_modify_rejects<Book>},
        {"random flow keeps invariants", test_random_flow_keeps_invariants<Book>},
        {"microstructure stats", test_stats<Book>},
    };
    for (const auto& c : cases) tests.push_back({tag + " " + c.name, c.fn});
}

int main() {
    std::vector<TestCase> tests;
    add_book_tests<OrderBook>(tests, "[v1]");
    add_book_tests<FastOrderBook>(tests, "[v2]");
    tests.push_back({"[csv] parse/format price", test_parse_price});
    tests.push_back({"[csv] parse line", test_parse_line});
    tests.push_back({"[msg] apply dispatch", test_apply_dispatch});
    tests.push_back({"[map] FlatIdMap vs unordered_map", test_flat_id_map});

    int failed_tests = 0;
    for (const auto& t : tests) {
        const int before = g_failures;
        t.fn();
        const bool ok = g_failures == before;
        failed_tests += !ok;
        std::printf("%s  %s\n", ok ? "[pass]" : "[FAIL]", t.name.c_str());
    }
    std::printf("\n%zu tests, %d checks, %d failures\n", tests.size(), g_checks, g_failures);
    return failed_tests == 0 ? 0 : 1;
}
