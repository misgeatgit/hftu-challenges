#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

namespace hftu {

class OrderBook {
public:
    static constexpr int SIDE_BID = 0;
    static constexpr int SIDE_ASK = 1;
    static constexpr int32_t MAX_TICKS = 10'000'000;
    
    // Used to mark an ID as empty/cancelled without deleting it
    static constexpr uint32_t INVALID_ORDER = 0xFFFFFFFF;

    OrderBook(size_t max_orders = 10'000'000);
    ~OrderBook() = default;

    void add_order(uint64_t id, int side, int64_t price, int64_t quantity);
    void cancel_order(uint64_t id);

    // Kept inline in the header for maximum O(1) query performance
    inline int64_t best_bid() const { return best_bid_price > 0 ? best_bid_price : 0; }
    inline int64_t best_ask() const { return best_ask_price < MAX_TICKS - 1 ? best_ask_price : 0; }

private:
    std::vector<uint32_t> order_info;
    std::vector<int32_t> bid_counts;
    std::vector<int32_t> ask_counts;
    std::vector<uint64_t> bid_bitmask;
    std::vector<uint64_t> ask_bitmask;

    int32_t best_bid_price = 0;
    int32_t best_ask_price = MAX_TICKS - 1;

    void update_best_bid(int32_t current_price);
    void update_best_ask(int32_t current_price);
};

} // namespace hftu
