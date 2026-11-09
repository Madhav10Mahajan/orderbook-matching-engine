# 04 — Design Decisions (agreed)

Each decision lists what we chose, why, and what we rejected. Answers to the
questions in `02_DESIGN_QUESTIONS.md` are collected here as we settle them.

## Q1 — Field types

| Field | Type | Why | Rejected |
|---|---|---|---|
| Order ID | `uint64_t` | Unique label; exchanges issue billions per day, so 64 bits. Never negative and no arithmetic on it, so unsigned is safe. | `int` overflows at ~2.1B. `double` — ids aren't fractional and doubles lose exact integers above 2^53. `long long` — works (same size on macOS) but signed, and doesn't state intent. |
| Quantity (one order) | `int32_t` | Whole shares; exchanges cap order size far below 2.1B (NASDAQ ITCH uses 32-bit share counts). Signed so a bug shows up as a negative value an `assert(qty > 0)` catches. Smaller `Order` → more orders per cache line. | `double` — no fractional shares, rounding on subtraction. `uint32_t` — `5 - 7` silently wraps to ~4 billion. `int` — only guaranteed ≥16 bits. |
| Quantity (aggregates: level total, volume) | `int64_t` | A sum of many orders can exceed 32 bits. | `int32_t` could overflow on a busy level. |
| Price | `int64_t`, in **ticks** (e.g. $501.37 with $0.01 tick → `50137`) | Exchange prices are integer multiples of the tick, so integers represent them exactly: map-key equality is correct and comparisons are one instruction. Signed because prices can be negative (WTI crude hit −$37.63 in April 2020; spread instruments). Convert to dollars only for I/O. | `double` — `501.30 + 0.07 != 501.37`, so one price can split into two map keys and break time priority. `uint64_t` — can't hold negative prices. |

| Side | `enum class Side : uint8_t { Buy, Sell }` | Exactly two legal values, enforced by the compiler; reads clearly at call sites (`Side::Buy`); 1 byte. `enum class` doesn't silently convert to `int`. Input `'B'`/`'S'` is converted once at parse time. | `bool isBuy` — `add(10, true, …)` is unreadable. `char` / `int` — `'X'` or `7` compile and become bugs. |

The side of an incoming order decides which map it matches against (BUY → asks,
SELL → bids), which map its remainder rests in, and whether it is the buyer or
seller in the trade report. A resting order's side is implied by the map it sits
in, but a cancel message carries only the id, so side + price must be findable
from the id (see Q4).

General rule: fixed-width types (`<cstdint>`) instead of `int` / `long long`, so each field's size and signedness is an explicit decision.

## Q2 — Price levels on each side

| Side | Structure | Best price |
|---|---|---|
| Asks | `std::map<int64_t, Level>` (ascending) | `begin()` = lowest |
| Bids | `std::map<int64_t, Level, std::greater<int64_t>>` (descending) | `begin()` = highest |

- `std::map` is a red-black tree: find / insert / erase a level in O(log M), M = distinct prices; best price is `begin()`, O(1).
- Flipping the comparator for bids (instead of reading `rbegin()`) means `begin()` is "best" on both sides, so the matching loop can be written once as a template.
- Rejected: heap (can't erase an arbitrary level), hash map alone (no ordering → no "next best price"), sorted vector (O(M) insert — revisit in the optimisation phase, since activity clusters near the top).

## Q3 — Orders within a level

```cpp
struct Order { OrderId id; Quantity qty; };
struct Level { TotalQuantity total_qty; std::list<Order> orders; };
```

`std::list` (doubly linked list): O(1) push-back, pop-front, **and erase from the
middle given an iterator**. Crucially, a `std::list` iterator stays valid when
*other* elements are inserted or erased, so an iterator saved when the order was
added still points at it later.

`total_qty` is stored and updated on add / fill / cancel, so depth queries are O(1)
per level instead of summing the queue.

Rejected:
- `std::deque` — erase from the middle is O(n), and *any* insert/erase in the middle
  invalidates all iterators, so we can't store a "bookmark" to an order.
- `std::vector` — same problems, plus O(n) pop-front.
- deque + lazy deletion (mark cancelled, skip later) — dead entries pile up
  (most orders are cancelled), matching walks over them, memory grows.

Known cost (for the optimisation phase): `std::list` allocates one heap node per
order and nodes are scattered in memory (poor cache locality). Fix later with a
preallocated pool / intrusive list.

## Q4 — Cancel: the order-id catalogue

```cpp
struct Location { Side side; Price price; std::list<Order>::iterator it; };
std::unordered_map<OrderId, Location> orders_by_id;
```

`cancel(id)`:
1. `orders_by_id.find(id)` — O(1) average. Not found → reject (filled / cancelled / never existed).
2. `side` picks the map, `price` finds the level — O(log M).
3. `level.total_qty -= it->qty; level.orders.erase(it);` — O(1).
4. Level empty → erase it from the map — O(log M) (amortised O(1) when erasing by iterator).
5. `orders_by_id.erase(id)` — O(1).

Invariant: an id is in `orders_by_id` **iff** that order is resting in the book.
So every path that removes a resting order — cancel *and* a full fill during
matching — must also erase it from `orders_by_id`.

Rejected: scanning every level to find the id (O(N) for the most frequent message);
`std::map` for the catalogue (O(log N) and ordering by id is never needed).

## Complexity (current design)

| Operation | Cost | Notes |
|---|---|---|
| Add, existing level | O(log M) | find level; push-back + catalogue insert O(1) |
| Add, new level | O(log M) | map insert |
| Cancel | O(log M) | O(1) catalogue + erase; O(log M) level lookup |
| One fill while matching | O(1) | front of the best level; erasing an emptied level via `begin()` is amortised O(1) |
| Best bid / ask | O(1) | `begin()` |
| Depth(n) | O(n) | walk first n levels, read `total_qty` |

M = number of distinct price levels on a side (small in practice — tens to hundreds).

## Q8 — Trade reporting

First version returns `std::vector<Trade>` from add/market calls (simple, easy to
test). May switch to a callback/listener in the optimisation phase to avoid the allocation.

## Q5 — Empty price levels

A level is erased the moment its last order leaves (fill or cancel), in both
books. So "best price" is always simply the first/last level, O(1). There are
never empty levels to skip; `check_invariants()` asserts this.

## Q6 — Memory ownership

- v1: the containers own everything (`std::list` nodes, map nodes, hash nodes).
  This is simple and safe, but means about 3 heap allocations per resting order.
- v2: one pooled `std::vector<Node>` owns all orders. Freed nodes go on a free list
  threaded through `Node::next` and are reused. Links are **indices, not pointers**,
  so the pool can grow (reallocate) without invalidating anything. In steady state
  there's no malloc/free on the hot path.

## Phase 6 — Time-in-force and modify

| Rule | Implementation | Why |
|---|---|---|
| GTC | match, then rest the remainder | default exchange behaviour |
| IOC | match, then discard the remainder (`rested = 0`) | taker that never wants to sit in the book |
| FOK | first sum the quantity at crossing levels (`fillable()`); if `< qty` → `Status::Killed`, no trades; else match | trades can't be undone, so the check must happen **before** matching. It costs one walk over the crossing levels, which stops early once enough is found |
| Modify, same price, qty ≤ current | shrink in place, keep queue position | makes the order less aggressive, so nobody behind it is disadvantaged (exchanges do the same) |
| Modify, price change or qty increase | cancel + re-add as GTC with the same id | would otherwise jump the queue; the new price may cross and trade |
| Modify to qty ≤ 0 | `InvalidQuantity` | use `cancel` to remove an order |

## Q8 — Trade reporting

`ExecResult { status, trades, rested }` is returned by value. `std::vector<Trade>` only
allocates when a fill happens, so the common case (a passive add or a cancel)
allocates nothing. A callback interface would remove even that; it's listed as future work.

## Q10 — Correctness strategy (Phase 5)

1. `check_invariants()` (O(N), tests only): not crossed; no empty levels; qty > 0;
   level total = sum of orders; every resting order's index entry points to exactly
   that order; index size = number of resting orders.
2. `ReferenceBook`: a flat vector with linear scans, correct by inspection.
3. Differential fuzzer: identical status, trades, rested qty, BBO, 64-level depth
   and order count after **every** message, over millions of messages, with
   deliberate edge cases.
4. ASan/UBSan on all test builds.

## Phase 9 — v2 `FastOrderBook`

| Component | v1 | v2 | Why it's faster |
|---|---|---|---|
| Price levels | `std::map` (red-black tree) | `std::vector<Level>` sorted worst → best, best at `back()` | contiguous binary search instead of pointer chasing; activity is near the top = end of vector, so inserts/erases shift few elements; `pop_back()` when the best level empties |
| Queue | `std::list<Order>` | intrusive doubly linked list of `uint32` indices in a pooled vector | no malloc per order; nodes reused hot from the free list |
| Id index | `std::unordered_map` | `FlatIdMap` (open addressing, linear probing, Fibonacci hashing, backward-shift deletion, load ≤ 0.5) | no node per entry; lookup is usually one cache line; backward shift avoids tombstone build-up under heavy add/cancel churn |

Trade-offs:
- Inserting a new level far from the top shifts O(M) elements. That's rare
  with realistic order flow, and M is small.
- `FlatIdMap` doubles (rehashes) when half full, which causes a single latency
  spike. Pre-size it in production.

Measured (Apple M4, 2M messages): throughput 13.0 → 29.7 M msg/s (2.3×);
p99 666 → 209 ns (3.2×); p99.9 1208 → 458 ns (2.6×). Full table in the README.

## Future work (deliberately out of scope)

Multiple instruments, networking (FIX/ITCH gateways), multithreading,
persistence and recovery, iceberg/stop orders, auctions, self-trade prevention,
a callback-based trade sink, a price-indexed array for O(1) level lookup when the
price range is bounded.
