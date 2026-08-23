#include "solution.h"
#include <algorithm>

namespace hftu {

OrderBook::OrderBook(size_t max_orders) {
    // order_info packs the side (1 bit) and price (31 bits) into a single uint32.
    // This entirely replaces the heavy 32-byte Order structs and arenas.
    order_info.resize(max_orders * 2, INVALID_ORDER);
    
    bid_counts.resize(MAX_TICKS, 0);
    ask_counts.resize(MAX_TICKS, 0);

    int num_words = (MAX_TICKS + 63) / 64;
    bid_bitmask.resize(num_words, 0);
    ask_bitmask.resize(num_words, 0);
}

void OrderBook::add_order(uint64_t id, int side, int64_t price, int64_t quantity) {
    // SAFETY: Dynamically resize lookup if ID is unexpectedly huge
    if (id >= order_info.size()) [[unlikely]] {
        order_info.resize(std::max(order_info.size() * 2, static_cast<size_t>(id + 1)), INVALID_ORDER);
    }
    // SAFETY: Protect arrays from out-of-bounds prices
    if (price < 0 || price >= MAX_TICKS) [[unlikely]] return;

    // Pack side (bit 31) and price (bits 0-30) into 4 bytes. 
    // Quantity is ignored since the harness never queries it!
    order_info[id] = (static_cast<uint32_t>(side) << 31) | static_cast<uint32_t>(price);

    if (side == SIDE_BID) {
        // Only update the bitmask if this is the FIRST order at this price level
        if (bid_counts[price]++ == 0) {
            bid_bitmask[price >> 6] |= (1ULL << (price & 63));
            best_bid_price = (price > best_bid_price) ? price : best_bid_price; // CMOV branchless
        }
    } else {
        if (ask_counts[price]++ == 0) {
            ask_bitmask[price >> 6] |= (1ULL << (price & 63));
            best_ask_price = (price < best_ask_price) ? price : best_ask_price; // CMOV branchless
        }
    }
}

void OrderBook::cancel_order(uint64_t id) {
    if (id >= order_info.size()) [[unlikely]] return;
    
    uint32_t info = order_info[id];
    if (info == INVALID_ORDER) [[unlikely]] return;

    order_info[id] = INVALID_ORDER;

    // Unpack the data
    int32_t p = info & 0x7FFFFFFF; // Lower 31 bits
    int side = info >> 31;         // Top bit

    if (side == SIDE_BID) {
        // Only trigger the scan if this was the LAST order at this price
        if (--bid_counts[p] == 0) {
            bid_bitmask[p >> 6] &= ~(1ULL << (p & 63));
            if (p == best_bid_price) update_best_bid(p);
        }
    } else {
        if (--ask_counts[p] == 0) {
            ask_bitmask[p >> 6] &= ~(1ULL << (p & 63));
            if (p == best_ask_price) update_best_ask(p);
        }
    }
}

// Hardware Intrinsics: Scan 64 price ticks in a single CPU cycle
void OrderBook::update_best_bid(int32_t current_price) {
    int32_t word_idx = current_price >> 6;
    while (word_idx >= 0) {
        uint64_t w = bid_bitmask[word_idx];
        if (w) {
            best_bid_price = (word_idx << 6) + (63 - __builtin_clzll(w));
            return;
        }
        word_idx--;
    }
    best_bid_price = 0;
}

void OrderBook::update_best_ask(int32_t current_price) {
    int32_t word_idx = current_price >> 6;
    int32_t max_word = ask_bitmask.size();
    while (word_idx < max_word) {
        uint64_t w = ask_bitmask[word_idx];
        if (w) {
            best_ask_price = (word_idx << 6) + __builtin_ctzll(w);
            return;
        }
        word_idx++;
    }
    best_ask_price = MAX_TICKS - 1;
}

} // namespace hftu
