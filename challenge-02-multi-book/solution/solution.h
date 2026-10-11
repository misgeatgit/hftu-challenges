#pragma once
// Challenge 02: Multi-Symbol Order Book (Optimized Header)

#include "../venue.h"
#include <cstdint>
#include <vector>

namespace hftu {

struct OrderNode {
    uint64_t exchange_id;
    uint64_t our_id;
    uint16_t symbol;
    int8_t side;
    int64_t price;
    int64_t qty;
    uint32_t prev_idx;
    uint32_t next_idx;
    bool active;
};

struct PriceLevel {
    int64_t price;
    int64_t total_qty;
    int32_t count;
    uint32_t head_order_idx;
    uint32_t tail_order_idx;
};

struct SymbolBookData {
    std::vector<PriceLevel> bids; // Sorted descending by price
    std::vector<PriceLevel> asks; // Sorted ascending by price
};

class MultiOrderBook {
public:
    explicit MultiOrderBook(Venue& venue);
    ~MultiOrderBook();

    // === Our order management ===
    void send_order(uint64_t our_id, uint16_t symbol, int side,
                    int64_t price, int64_t qty);
    void modify_our_order(uint64_t our_id, int64_t new_price, int64_t new_qty);
    void cancel_our_order(uint64_t our_id);

    // === Exchange feed ===
    void add_order(uint64_t exchange_id, uint16_t symbol, int side,
                   int64_t price, int64_t qty);
    void modify_order(uint64_t exchange_id, int64_t new_qty);
    void cancel_order(uint64_t exchange_id);

    // === Queries ===
    TopLevel best_bid(uint16_t symbol) const;
    TopLevel best_ask(uint16_t symbol) const;
    int get_top_levels(uint16_t symbol, int side, int n, TopLevel* out) const;
    int64_t volume_near_best(uint16_t symbol, int side, int64_t depth) const;
    QueuePosition get_queue_position(uint64_t our_id) const;

private:
    Venue& venue_;

    struct HashEntry {
        uint64_t key;
        uint32_t value;
        bool occupied;
    };

    static constexpr size_t HASH_CAPACITY = 2'097'152; // 2M slots
    HashEntry* order_hash_;
    
    struct OurHashEntry {
        uint64_t our_id;
        uint64_t exchange_id;
        bool occupied;
    };
    OurHashEntry* our_hash_;
    static constexpr size_t OUR_HASH_CAPACITY = 524'288; // 512K slots

    std::vector<OrderNode> order_pool_;
    uint32_t free_order_head_;

    SymbolBookData books_[NUM_SYMBOLS];

    uint32_t allocate_order();
    void free_order(uint32_t idx);
    uint32_t find_order(uint64_t exchange_id) const;
    void insert_order_hash(uint64_t exchange_id, uint32_t order_idx);
    void remove_order_hash(uint64_t exchange_id);

    uint64_t find_our_exchange_id(uint64_t our_id) const;
    void insert_our_mapping(uint64_t our_id, uint64_t exchange_id);
    void remove_our_mapping(uint64_t our_id);

    PriceLevel* find_or_create_level(uint16_t symbol, int side, int64_t price);
    PriceLevel* find_level(uint16_t symbol, int side, int64_t price);
    void remove_level_if_empty(uint16_t symbol, int side, int64_t price);
};

} // namespace hftu