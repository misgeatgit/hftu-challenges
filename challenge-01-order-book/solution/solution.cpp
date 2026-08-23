#include "solution.h"
#include <algorithm>

namespace hftu {

OrderBook::OrderBook(size_t max_orders) {
    best_prices_[0] = 0;
    best_prices_[1] = MAX_TICKS - 1;

    // Raw heap allocations bypass all C++ std::vector bounds-checking overhead.
    // The () ensures the memory is zero-initialized.
    order_info_ = new uint32_t[max_orders]();
    
    counts_[0] = new int32_t[MAX_TICKS]();
    counts_[1] = new int32_t[MAX_TICKS]();

    int num_words = (MAX_TICKS + 63) / 64;
    bitmasks_[0] = new uint64_t[num_words]();
    bitmasks_[1] = new uint64_t[num_words]();
}

OrderBook::~OrderBook() {
    delete[] order_info_;
    delete[] counts_[0];
    delete[] counts_[1];
    delete[] bitmasks_[0];
    delete[] bitmasks_[1];
}

void OrderBook::add_order(uint64_t id, int side, int64_t price, int64_t quantity) {
    // 1. Pack the side and price into 32 bits
    order_info_[id] = (static_cast<uint32_t>(side) << 31) | static_cast<uint32_t>(price);

    // 2. Branchless Array Selection:
    // We use `side` (0 or 1) as the pointer index. No "if (side == SIDE_BID)" needed.
    // This evaluates to the exact memory address in 1 cycle.
    if (counts_[side][price]++ == 0) [[unlikely]] {
        
        bitmasks_[side][price >> 6] |= (1ULL << (price & 63));
        
        // This is the ONLY branch, and it is executed < 1% of the time 
        // (only when a completely empty price level receives its first order).
        if (side == SIDE_BID) {
            best_prices_[0] = std::max(best_prices_[0], static_cast<int32_t>(price));
        } else {
            best_prices_[1] = std::min(best_prices_[1], static_cast<int32_t>(price));
        }
    }
}

void OrderBook::cancel_order(uint64_t id) {
    uint32_t info = order_info_[id];
    
    // Unpack in 2 CPU cycles
    uint32_t p = info & 0x7FFFFFFF;
    uint32_t side = info >> 31;

    // Branchless lookup and decrement. 
    if (--counts_[side][p] == 0) [[unlikely]] {
        
        bitmasks_[side][p >> 6] &= ~(1ULL << (p & 63));
        
        if (p == best_prices_[side]) {
            update_best(side, p);
        }
    }
}

void OrderBook::update_best(uint32_t side, int32_t current_price) {
    if (side == SIDE_BID) {
        int32_t word_idx = current_price >> 6;
        while (word_idx >= 0) {
            uint64_t w = bitmasks_[0][word_idx];
            if (w) {
                best_prices_[0] = (word_idx << 6) + (63 - __builtin_clzll(w));
                return;
            }
            word_idx--;
        }
        best_prices_[0] = 0;
    } else {
        int32_t word_idx = current_price >> 6;
        int32_t max_word = (MAX_TICKS + 63) / 64;
        while (word_idx < max_word) {
            uint64_t w = bitmasks_[1][word_idx];
            if (w) {
                best_prices_[1] = (word_idx << 6) + __builtin_ctzll(w);
                return;
            }
            word_idx++;
        }
        best_prices_[1] = MAX_TICKS - 1;
    }
}

} // namespace hftu
