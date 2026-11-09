# 02 — Design Questions (YOUR homework)

These are the decisions **you** make. No answers here on purpose. For each one,
write down: your choice, the time complexity of every operation it affects, and
one alternative you rejected and why. That written reasoning is exactly what an
interviewer will probe.

Fill in your answers in the `My answer:` lines, then we'll code it together.

---

### Workload facts to design around
- Real exchange traffic is **dominated by adds and cancels**; trades are a small
  fraction. Many orders are cancelled within milliseconds of being placed.
- Most activity happens **near the top of the book** (within a few ticks of the best price).
- The two queries everything depends on: **"what is the best price?"** and
  **"who is first in line at that price?"**
- Prices live on a **discrete grid** (tick size). Order ids are unique integers.

---

## Q1. How do you represent a price?
`101.37` — what type? Think about: comparing two prices for equality, `0.1 + 0.2`,
and whether prices are really continuous.

My answer:

## Q2. How do you store the price levels on ONE side of the book?
You need: find the best price, insert a new price level, delete an empty level,
find a specific level by price.
- What are the costs of each with your structure?
- Do bids and asks need different orderings? How do you handle that?

My answer:

## Q3. How do you store the orders WITHIN one price level?
You need: append a newcomer at the back, take from the front when matching,
and remove one from the **middle** when it's cancelled.

My answer:

## Q4. How do you make `cancel(order_id)` fast?
Given only an id, how do you find that order without scanning the book?
What exactly do you store for each id?

My answer:

## Q5. What happens when a price level becomes empty?
And how do you then find the *next* best price?

My answer:

## Q6. Who owns the order's memory?
`new`/`delete` per order? Smart pointers? Something preallocated? What does each
cost on the hot path, and what can go wrong (dangling references after cancel)?

My answer:

## Q7. Write the matching algorithm in pseudocode
Incoming BUY with limit price P and qty Q. Loop until … ? Handle: partial fill of
a resting order, full fill, emptying a level, market orders (no limit), IOC, FOK.
(Hint for FOK: you need to know something **before** you start trading.)

My answer:

## Q8. How do trades get reported to the outside world?
Return a vector? Callback/listener? What does each cost per order?

My answer:

## Q9. Complexity table — fill this in for your design

| Operation | Time complexity | Why |
|---|---|---|
| Add (new price level) | | |
| Add (existing price level) | | |
| Cancel | | |
| Match one fill | | |
| Best bid / ask | | |
| Depth(n) snapshot | | |

## Q10. How will you PROVE it's correct?
Unit tests are one part. What's a "reference implementation" you could compare
against on random input? What invariants (see 01, §5) do you check after every op?

My answer:

---

### Bonus questions (Tier 3 — think about later)
- If prices only move within ±X ticks of a reference, is there a structure with
  O(1) best-price lookup *and* O(1) insert? What's the trade-off?
- Why might a `std::vector` beat a "theoretically better" linked structure in practice? (cache)
- How would you measure latency of a single operation that takes ~50 ns?
  (Clock overhead, warm-up, percentiles vs averages.)
