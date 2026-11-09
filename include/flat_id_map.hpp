// Open-addressing hash map from OrderId to a 32-bit index (linear probing).
//
// Why not std::unordered_map: it allocates one heap node per entry and every
// lookup chases a pointer into a bucket list. Here all entries live in one flat
// array, so an insert never allocates (after growth) and a lookup is usually a
// single cache line. Deletion uses backward-shift instead of tombstones, so
// heavy add/cancel churn doesn't slowly fill the table with dead slots.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "types.hpp"

namespace ob {

class FlatIdMap {
public:
    static constexpr std::uint32_t kNotFound = UINT32_MAX;

    explicit FlatIdMap(std::size_t initial_capacity = 1024) { rehash(round_up_pow2(initial_capacity)); }

    std::size_t size() const { return size_; }

    std::uint32_t find(OrderId key) const {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (!s.used) return kNotFound;
            if (s.key == key) return s.value;
        }
    }

    bool contains(OrderId key) const { return find(key) != kNotFound; }

    // Precondition: key is not present.
    void insert(OrderId key, std::uint32_t value) {
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);  // load factor <= 0.5
        place(key, value);
        ++size_;
    }

    bool erase(OrderId key) {
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            if (!slots_[i].used) return false;
            if (slots_[i].key == key) break;
        }
        // Backward shift: pull later entries of the same probe run into the hole,
        // so every remaining key is still reachable from its home slot.
        std::size_t hole = i;
        for (std::size_t j = (hole + 1) & mask_; slots_[j].used; j = (j + 1) & mask_) {
            const std::size_t h = home(slots_[j].key);
            // Entry j may move into the hole only if its home is NOT in (hole, j] cyclically.
            const bool home_between = hole <= j ? (hole < h && h <= j) : (hole < h || h <= j);
            if (!home_between) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].used = false;
        --size_;
        return true;
    }

    template <class Fn>
    void for_each(Fn&& fn) const {
        for (const Slot& s : slots_)
            if (s.used) fn(s.key, s.value);
    }

private:
    struct Slot {
        OrderId key = 0;
        std::uint32_t value = 0;
        bool used = false;
    };

    static std::size_t round_up_pow2(std::size_t n) {
        std::size_t p = 16;
        while (p < n) p <<= 1;
        return p;
    }

    // Fibonacci hashing: multiply spreads sequential ids across the table.
    std::size_t home(OrderId key) const {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void place(OrderId key, std::uint32_t value) {
        std::size_t i = home(key);
        while (slots_[i].used) i = (i + 1) & mask_;
        slots_[i] = Slot{key, value, true};
    }

    void rehash(std::size_t capacity) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(capacity, Slot{});
        mask_ = capacity - 1;
        shift_ = 64;
        for (std::size_t c = capacity; c > 1; c >>= 1) --shift_;
        for (const Slot& s : old)
            if (s.used) place(s.key, s.value);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    std::size_t size_ = 0;
};

}  // namespace ob
