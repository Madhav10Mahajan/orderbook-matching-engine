# 01 — Problem Statement: What Are We Building?

## The one-line pitch

> A **limit order book and matching engine** in modern C++ that accepts buy/sell
> orders, matches them by **price-time priority**, reports trades and book state,
> and is **verified for correctness** and **benchmarked for latency** on a
> realistic stream of millions of orders.

That's the sentence for your resume and for the first 10 seconds of an interview.

---

## 1. Background — what is an order book?

Every exchange (NASDAQ, NSE, CME, Binance) keeps, for each instrument, a list of
everyone who currently wants to buy or sell and at what price. That list is the
**limit order book (LOB)**.

```
        ASKS (sellers)                       Best ask = 101.00
   price   qty   orders (oldest → newest)
   102.00   10   [C:10]
   101.00    8   [A:5, B:3]        ← lowest ask = "best ask" / "offer"
   ───────────────────────────────  spread = 101.00 − 100.50 = 0.50
   100.50    4   [D:4]             ← highest bid = "best bid"
   100.00   15   [E:10, F:5]
        BIDS (buyers)                        Best bid = 100.50
```

Vocabulary you must be able to say fluently:

| Term | Meaning |
|---|---|
| **Bid / Ask** | Buy side / sell side of the book |
| **Best bid / best ask (BBO, "top of book")** | Highest price someone will pay / lowest price someone will sell |
| **Spread** | best ask − best bid |
| **Mid price** | (best bid + best ask) / 2 |
| **Price level** | All resting orders at one exact price |
| **Depth** | Total quantity at each price level (L2 data) |
| **Tick size** | Smallest allowed price increment (e.g. $0.01) |
| **Resting order** | A limit order sitting in the book waiting to be matched (a *maker*, provides liquidity) |
| **Aggressive order** | An incoming order that crosses the spread and trades immediately (a *taker*, removes liquidity) |
| **L1 / L2 / L3 data** | L1 = BBO only; L2 = aggregated qty per price level; L3 = every individual order |

## 2. The matching rule: price-time priority (FIFO)

When an incoming order can trade, it matches against the opposite side by:

1. **Price priority** — the best price goes first (lowest ask for a buyer, highest bid for a seller).
2. **Time priority** — at the same price, the order that arrived **first** goes first.
3. Trades execute at the **resting order's price**, not the incoming order's price.

### Worked example (use this as your first unit test)

Book above. A new order arrives: **BUY 12 @ limit 102.00** (id G).

| Step | Matches against | Fill | Price | G remaining |
|---|---|---|---|---|
| 1 | A (101.00, oldest) | 5 | 101.00 | 7 |
| 2 | B (101.00) | 3 | 101.00 | 4 |
| 3 | C (102.00) | 4 | 102.00 | 0 |

Output: 3 trades. G is fully filled, nothing of G rests. C stays in the book with 6.
The 101.00 level is now empty and disappears; new best ask = 102.00.

If G had been **BUY 12 @ 101.00**, it would fill 8 (A, B) and the remaining **4
would rest on the bid side at 101.00** — becoming the new best bid.

## 3. The objective (what "done" means)

Build a single-instrument order book engine that:

1. **Is correct** — matches exactly by price-time priority, never loses or
   double-counts quantity, never leaves a *crossed book* (best bid ≥ best ask) after
   processing an order.
2. **Is fast** — every core operation runs in (amortised) constant or logarithmic time,
   with no heap allocation on the hot path in the optimised version. We measure it.
3. **Is measurable** — a benchmark harness reports throughput (orders/sec) and
   latency percentiles (p50 / p99 / p99.9) for a reproducible workload.
4. **Is explainable** — every data-structure choice has a written justification and
   a complexity analysis (this is the part MFE interviewers care about most).
5. **Connects to finance** — computes microstructure stats (spread, mid, order-book
   imbalance, VWAP) from the book, which ties the engineering to quant research.

## 4. Feature list (tiered)

### Tier 1 — Core engine (must have)
| # | Feature | Description |
|---|---|---|
| F1 | **Add limit order** | `add(id, side, price, qty)` — match what crosses, rest the remainder |
| F2 | **Cancel order** | `cancel(id)` — remove a resting order by its id |
| F3 | **Market order** | Trade at any price until filled or the opposite side is empty; never rests |
| F4 | **Matching** | Price-time priority, partial fills, multi-level sweeps |
| F5 | **Trade reports** | Every fill emits `{buy_id, sell_id, price, qty}` |
| F6 | **Queries** | best bid, best ask, spread, mid, qty at a price, top-N depth (L2 snapshot) |
| F7 | **Input validation** | Reject zero/negative qty, bad price, duplicate id, cancel of unknown id |

### Tier 2 — Realism (should have)
| # | Feature | Description |
|---|---|---|
| F8 | **Modify order** | Change qty/price. Rule to implement: *reducing qty keeps time priority; changing price or increasing qty loses it* (cancel + re-add) |
| F9 | **Time-in-force** | **GTC** (rest until cancelled), **IOC** (fill what you can now, cancel the rest), **FOK** (fill entirely now or do nothing) |
| F10 | **Event stream** | Book emits events (ack, reject, fill, cancel) — a callback / listener interface |
| F11 | **Replay from file** | Read a CSV of orders, run them through, write trades to a CSV |

### Tier 3 — Performance & analysis (what makes it stand out)
| # | Feature | Description |
|---|---|---|
| F12 | **Benchmark harness** | Synthetic random order flow (configurable mix of add/cancel/market), reports ops/sec + latency histogram |
| F13 | **Optimised version** | Remove allocations on hot path (object pool / preallocated memory), cache-friendly layout; show before/after numbers |
| F14 | **Microstructure stats** | Spread, mid, order-book imbalance `(bidQty − askQty)/(bidQty + askQty)`, VWAP of trades |
| F15 | **Real data replay** | Replay LOBSTER (or NASDAQ ITCH) messages for one stock-day and compare our reconstructed top-of-book to theirs |

### Out of scope (say this in interviews — scoping is a skill)
Networking/FIX protocol, multiple instruments + routing, persistence/recovery,
multithreading, hidden/iceberg/stop orders, auctions. Each is a possible "future work" bullet.

## 5. Interface contract (the "what", not the "how")

The engine must expose roughly this behaviour. **How** it stores things internally
is your decision — see `02_DESIGN_QUESTIONS.md`.

```
add_limit(order_id, side, price, quantity, tif = GTC) -> list of trades (or via callback)
add_market(order_id, side, quantity)                  -> list of trades
cancel(order_id)                                      -> success / not found
modify(order_id, new_price, new_quantity)             -> success / not found / trades
best_bid() / best_ask()                               -> price + total qty, or "empty"
depth(n)                                              -> top n levels per side
```

**Invariants that must hold after every call** (we will assert these in tests):
- Book is never crossed: `best_bid < best_ask` whenever both sides are non-empty.
- Every resting order has `qty > 0`; no empty price levels exist.
- Sum of order quantities at a level == the level's reported quantity.
- Total quantity is conserved: qty submitted = 2 × qty traded (both sides of each fill) + qty resting + qty cancelled + qty discarded (unfilled IOC/market remainder).
- Order ids in the book are unique.

## 6. Input / output formats (for replay & tests)

Input CSV (one event per line):
```
type,id,side,price,qty,tif
A,1,S,101.00,5,GTC      # add limit
A,2,S,101.00,3,GTC
M,3,B,,4,               # market order
C,1,,,,                 # cancel
X,2,,101.50,2,          # modify
```
Output CSV of trades:
```
trade_seq,buy_id,sell_id,price,qty
```

## 7. Success metrics (what goes on the resume)

- Correctness: N unit tests + randomized test comparing against a slow, obviously-correct reference book.
- Throughput: "X million orders/sec" on M-series Mac, single thread.
- Latency: "p50 Y ns, p99 Z ns per add/cancel".
- Optimisation story: "reduced p99 latency by K× by replacing … with …".

Resume bullet template (we fill numbers at the end):
> *Built a price-time priority limit order book & matching engine in C++17 supporting
> limit/market/IOC/FOK orders with O(1) cancel; validated against a reference model
> via randomized testing and benchmarked at **X M orders/s** with **p99 latency of Y ns**;
> replayed NASDAQ LOBSTER data to compute spread, imbalance and VWAP.*
