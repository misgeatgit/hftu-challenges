#pragma once
#include <cstdint>
#include <cstddef>

namespace hftu {

class OrderBook {
public:
    static constexpr int SIDE_BID = 0;
    static constexpr int SIDE_ASK = 1;
    static constexpr int32_t MAX_TICKS = 10'000'000;

    // Default sized massively to 30 Million to guarantee we never 
    // need bounds-checking in the hot path.
    OrderBook(size_t max_orders = 30'000'000);
    ~OrderBook();

    void add_order(uint64_t id, int side, int64_t price, int64_t quantity);
    void cancel_order(uint64_t id);

    inline int64_t best_bid() const { return best_prices_[0] > 0 ? best_prices_[0] : 0; }
    inline int64_t best_ask() const { return best_prices_[1] < MAX_TICKS - 1 ? best_prices_[1] : 0; }

private:
    // __restrict__ tells the compiler these pointers never overlap in memory,
    // allowing it to reorder assembly instructions for maximum IPC (Instructions Per Cycle).
    uint32_t* __restrict__ order_info_;
    
    // 2D Arrays: Index 0 is Bids, Index 1 is Asks.
    int32_t* __restrict__ counts_[2];
    uint64_t* __restrict__ bitmasks_[2];
    
    int32_t best_prices_[2];

    void update_best(uint32_t side, int32_t current_price);
};

} // namespace hftu
