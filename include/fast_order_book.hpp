// v2: the optimised order book. Same interface and identical behaviour to OrderBook
// (v1); the fuzzer checks both against the reference model. What changed and why
// is in docs/04_DESIGN_DECISIONS.md ("Phase 9").
//
//   v1                                   v2
//   std::list<Order> per level      ->   intrusive doubly linked list of indices into
//   (one malloc per order)               one pooled std::vector<Node> + free list
//   std::map<Price, Level>          ->   std::vector<Level> sorted worst -> best,
//   (one malloc per level)               best level at back()
//   std::unordered_map<Id,Location> ->   FlatIdMap (open addressing, no per-entry malloc)
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "flat_id_map.hpp"
#include "types.hpp"

namespace ob {

class FastOrderBook {
public:
    static constexpr const char* kName = "v2 pool + flat map + vector levels";

    explicit FastOrderBook(std::size_t expected_orders = 1 << 16);

    ExecResult add_limit(OrderId id, Side side, Price price, Quantity qty,
                         TimeInForce tif = TimeInForce::GTC);
    ExecResult add_market(OrderId id, Side side, Quantity qty);
    Status cancel(OrderId id);
    ExecResult modify(OrderId id, Price new_price, Quantity new_qty);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    TotalQuantity volume_at(Side side, Price price) const;
    std::vector<LevelInfo> depth(Side side, std::size_t n) const;
    std::optional<Quantity> order_qty(OrderId id) const;
    std::size_t order_count() const { return index_.size(); }

    std::string check_invariants() const;

private:
    static constexpr std::uint32_t kNil = UINT32_MAX;

    // An order and its queue links. Links are indices into nodes_, not pointers,
    // so the pool vector can grow without invalidating anything.
    struct Node {
        OrderId id;
        Price price;
        Quantity qty;
        std::uint32_t prev;
        std::uint32_t next;
        Side side;
    };

    struct Level {
        Price price;
        TotalQuantity total_qty;
        std::uint32_t head;  // oldest order (fills first)
        std::uint32_t tail;  // newest order
        std::uint32_t count;
    };

    // One side of the book. Levels are sorted worst -> best so the best price is
    // back(): most activity is near the top, so inserts/erases shift few elements,
    // and a binary search over a contiguous array is cache-friendly.
    template <class Better>
    struct BookSide {
        std::vector<Level> levels;

        static bool better(Price a, Price b) { return Better{}(a, b); }
        Level* find(Price price);
        const Level* find(Price price) const;
        Level& find_or_insert(Price price);
        void erase(Level* level) { levels.erase(levels.begin() + (level - levels.data())); }
    };
    using Bids = BookSide<std::greater<Price>>;  // higher is better
    using Asks = BookSide<std::less<Price>>;     // lower is better

    template <class SideBook>
    void match(SideBook& opposite, Side aggressor, OrderId id, Quantity& remaining,
               std::optional<Price> limit, std::vector<Trade>& trades);

    template <class SideBook>
    static TotalQuantity fillable(const SideBook& opposite, Price limit, Quantity wanted);

    template <class SideBook>
    void rest(SideBook& book, Side side, OrderId id, Price price, Quantity qty);

    template <class SideBook>
    void remove(SideBook& book, std::uint32_t idx);

    template <class SideBook>
    static std::vector<LevelInfo> depth_of(const SideBook& book, std::size_t n);

    template <class SideBook>
    std::string check_side(const SideBook& book, Side side, std::size_t& visited) const;

    std::uint32_t alloc_node();
    void free_node(std::uint32_t idx);
    void unlink(Level& level, std::uint32_t idx);

    Bids bids_;
    Asks asks_;
    std::vector<Node> nodes_;        // the pool
    std::uint32_t free_head_ = kNil; // free list threaded through Node::next
    FlatIdMap index_;                // order id -> node index
};

}  // namespace ob
