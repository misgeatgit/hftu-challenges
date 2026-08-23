#include "solution.h"
#include <algorithm>

namespace hftu {

OrderBook::OrderBook(size_t initial_orders) {
    best_prices_[0] = 0;
    best_prices_[1] = MAX_TICKS - 1;

    // Delegate memory management to std::vector
    order_info_vec_.resize(initial_orders, INVALID_IDX);
    
    // Cache the raw pointer and size to bypass STL overhead in the hot path
    order_info_ = order_info_vec_.data();
    max_id_ = initial_orders;

    counts_ = new uint16_t[MAX_TICKS * 2]();
    
    counts_[0] = 1; // Bid Sentinel
    counts_[(1 << TICK_SHIFT) | (MAX_TICKS - 1)] = 1; // Ask Sentinel

    int num_words = (MAX_TICKS * 2 + 63) / 64;
    bitmasks_ = new uint64_t[num_words]();
}

OrderBook::~OrderBook() {
    delete[] counts_;
    delete[] bitmasks_;
}

void OrderBook::add_order(uint64_t id, int side, int64_t price, int64_t quantity) {
    // SAFETY: Prevent memory bleed if the harness sends an invalid massive price
    if (price < 0 || price >= MAX_TICKS) [[unlikely]] return;

    // SAFETY: Dynamically scale without crashing if ID is larger than expected
    if (id >= max_id_) [[unlikely]] {
        order_info_vec_.resize(std::max(max_id_ * 2, static_cast<size_t>(id + 1)), INVALID_IDX);
        order_info_ = order_info_vec_.data(); // Re-cache the pointer!
        max_id_ = order_info_vec_.size();
    }

    uint32_t idx = (static_cast<uint32_t>(side) << TICK_SHIFT) | static_cast<uint32_t>(price);
    order_info_[id] = idx;

    if (counts_[idx]++ == 0) [[unlikely]] {
        bitmasks_[idx >> 6] |= (1ULL << (idx & 63));
        
        if (side == SIDE_BID) {
            if (price > best_prices_[0]) best_prices_[0] = price;
        } else {
            if (price < best_prices_[1]) best_prices_[1] = price;
        }
    }
}

void OrderBook::cancel_order(uint64_t id) {
    // SAFETY: Instantly reject out-of-bounds IDs from the harness
    if (id >= max_id_) [[unlikely]] return;

    uint32_t idx = order_info_[id];
    
    // SAFETY: Instantly reject double-cancels
    if (idx == INVALID_IDX) [[unlikely]] return; 
    
    order_info_[id] = INVALID_IDX; 
    
    if (--counts_[idx] == 0) [[unlikely]] {
        bitmasks_[idx >> 6] &= ~(1ULL << (idx & 63));
    }
}

// -------------------------------------------------------------------------
// LAZY EVALUATION SLOW PATHS 
// -------------------------------------------------------------------------
int64_t OrderBook::scan_best_bid(int32_t p) const {
    int32_t word_idx = p >> 6;
    while (word_idx >= 0) {
        uint64_t w = bitmasks_[word_idx];
        if (w) {
            p = (word_idx << 6) + (63 - __builtin_clzll(w));
            best_prices_[0] = p;
            return p;
        }
        word_idx--;
    }
    
    best_prices_[0] = 0;
    return 0;
}

int64_t OrderBook::scan_best_ask(int32_t p) const {
    uint32_t base_word = (1 << (TICK_SHIFT - 6));
    int32_t word_offset = p >> 6;
    int32_t max_offset = MAX_TICKS >> 6;

    while (word_offset < max_offset) {
        uint64_t w = bitmasks_[base_word + word_offset];
        if (w) {
            p = (word_offset << 6) + __builtin_ctzll(w);
            best_prices_[1] = p;
            return p;
        }
        word_offset++;
    }
    
    best_prices_[1] = MAX_TICKS - 1;
    return 0;
}

} // namespace hftu
