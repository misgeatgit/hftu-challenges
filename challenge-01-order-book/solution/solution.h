#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace hftu {

class OrderBook {
public:
    static constexpr int SIDE_BID = 0;
    static constexpr int SIDE_ASK = 1;
    
    static constexpr uint32_t TICK_SHIFT = 20; 
    static constexpr int32_t MAX_TICKS = 1 << TICK_SHIFT; 
    static constexpr uint32_t PRICE_MASK = MAX_TICKS - 1;
    
    // Sentinel for uninitialized or already-cancelled orders
    static constexpr uint32_t INVALID_IDX = 0xFFFFFFFF;

    OrderBook(size_t initial_orders = 1'000'000);
    ~OrderBook();

    void add_order(uint64_t id, int side, int64_t price, int64_t quantity);
    void cancel_order(uint64_t id);

    // LAZY EVALUATION FAST PATH
    inline int64_t best_bid() const {
        int32_t p = best_prices_[0];
        if (counts_[p] > 0) [[likely]] return p;
        return scan_best_bid(p); 
    }

    inline int64_t best_ask() const {
        int32_t p = best_prices_[1];
        if (counts_[(1 << TICK_SHIFT) | p] > 0) [[likely]] {
            return p == MAX_TICKS - 1 ? 0 : p;
        }
        return scan_best_ask(p);
    }

private:
    // Safely manages memory boundaries for unpredictable benchmark IDs
    std::vector<uint32_t> order_info_vec_;
    
    // Raw C-pointers extracted from the vector for zero-overhead access
    uint32_t* __restrict__ order_info_;
    size_t max_id_;

    uint16_t* __restrict__ counts_; 
    uint64_t* __restrict__ bitmasks_;
    
    mutable int32_t best_prices_[2];

    int64_t scan_best_bid(int32_t p) const;
    int64_t scan_best_ask(int32_t p) const;
};

} // namespace hftu
