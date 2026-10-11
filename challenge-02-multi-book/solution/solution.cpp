#include "solution.h"
#include <algorithm>
#include <cstring>

namespace hftu {

MultiOrderBook::MultiOrderBook(Venue& venue) : venue_(venue) {
    // Pre-allocate hash tables with zero allocations during timed phase
    order_hash_ = new HashEntry[HASH_CAPACITY]();
    our_hash_ = new OurHashEntry[OUR_HASH_CAPACITY]();

    // Pre-allocate order pool (e.g. 1.5 million max orders)
    order_pool_.resize(1'500'000);
    free_order_head_ = 0;
    for (size_t i = 0; i < order_pool_.size(); ++i) {
        order_pool_[i].next_idx = static_cast<uint32_t>(i + 1);
        order_pool_[i].active = false;
    }
    order_pool_.back().next_idx = 0xFFFFFFFF;

    for (int i = 0; i < NUM_SYMBOLS; ++i) {
        books_[i].bids.reserve(256);
        books_[i].asks.reserve(256);
    }
}

MultiOrderBook::~MultiOrderBook() {
    delete[] order_hash_;
    delete[] our_hash_;
}

uint32_t MultiOrderBook::allocate_order() {
    if (free_order_head_ == 0xFFFFFFFF) return 0xFFFFFFFF;
    uint32_t idx = free_order_head_;
    free_order_head_ = order_pool_[idx].next_idx;
    order_pool_[idx].active = true;
    return idx;
}

void MultiOrderBook::free_order(uint32_t idx) {
    order_pool_[idx].active = false;
    order_pool_[idx].next_idx = free_order_head_;
    free_order_head_ = idx;
}

// Open-addressing hash table lookup for exchange_id
uint32_t MultiOrderBook::find_order(uint64_t exchange_id) const {
    size_t idx = exchange_id & (HASH_CAPACITY - 1);
    size_t start_idx = idx;
    while (order_hash_[idx].occupied) {
        if (order_hash_[idx].key == exchange_id) {
            return order_hash_[idx].value;
        }
        idx = (idx + 1) & (HASH_CAPACITY - 1);
        if (idx == start_idx) break;
    }
    return 0xFFFFFFFF;
}

void MultiOrderBook::insert_order_hash(uint64_t exchange_id, uint32_t order_idx) {
    size_t idx = exchange_id & (HASH_CAPACITY - 1);
    while (order_hash_[idx].occupied) {
        idx = (idx + 1) & (HASH_CAPACITY - 1);
    }
    order_hash_[idx] = {exchange_id, order_idx, true};
}

void MultiOrderBook::remove_order_hash(uint64_t exchange_id) {
    size_t idx = exchange_id & (HASH_CAPACITY - 1);
    size_t start_idx = idx;
    while (order_hash_[idx].occupied) {
        if (order_hash_[idx].key == exchange_id) {
            order_hash_[idx].occupied = false;
            return;
        }
        idx = (idx + 1) & (HASH_CAPACITY - 1);
        if (idx == start_idx) break;
    }
}

// Our ID mapping hash table
uint64_t MultiOrderBook::find_our_exchange_id(uint64_t our_id) const {
    size_t idx = our_id & (OUR_HASH_CAPACITY - 1);
    size_t start_idx = idx;
    while (our_hash_[idx].occupied) {
        if (our_hash_[idx].our_id == our_id) {
            return our_hash_[idx].exchange_id;
        }
        idx = (idx + 1) & (OUR_HASH_CAPACITY - 1);
        if (idx == start_idx) break;
    }
    return 0;
}

void MultiOrderBook::insert_our_mapping(uint64_t our_id, uint64_t exchange_id) {
    size_t idx = our_id & (OUR_HASH_CAPACITY - 1);
    while (our_hash_[idx].occupied) {
        idx = (idx + 1) & (OUR_HASH_CAPACITY - 1);
    }
    our_hash_[idx] = {our_id, exchange_id, true};
}

void MultiOrderBook::remove_our_mapping(uint64_t our_id) {
    size_t idx = our_id & (OUR_HASH_CAPACITY - 1);
    size_t start_idx = idx;
    while (our_hash_[idx].occupied) {
        if (our_hash_[idx].our_id == our_id) {
            our_hash_[idx].occupied = false;
            return;
        }
        idx = (idx + 1) & (OUR_HASH_CAPACITY - 1);
        if (idx == start_idx) break;
    }
}

// Price level management
PriceLevel* MultiOrderBook::find_level(uint16_t symbol, int side, int64_t price) {
    auto& levels = (side == 0) ? books_[symbol].bids : books_[symbol].asks;
    auto it = std::lower_bound(levels.begin(), levels.end(), price, 
        [side](const PriceLevel& lvl, int64_t p) {
            return (side == 0) ? (lvl.price > p) : (lvl.price < p);
        });
    if (it != levels.end() && it->price == price) {
        return &(*it);
    }
    return nullptr;
}

PriceLevel* MultiOrderBook::find_or_create_level(uint16_t symbol, int side, int64_t price) {
    auto& levels = (side == 0) ? books_[symbol].bids : books_[symbol].asks;
    auto it = std::lower_bound(levels.begin(), levels.end(), price, 
        [side](const PriceLevel& lvl, int64_t p) {
            return (side == 0) ? (lvl.price > p) : (lvl.price < p);
        });
    if (it != levels.end() && it->price == price) {
        return &(*it);
    }
    PriceLevel new_lvl{price, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF};
    auto inserted = levels.insert(it, new_lvl);
    return &(*inserted);
}

void MultiOrderBook::remove_level_if_empty(uint16_t symbol, int side, int64_t price) {
    auto& levels = (side == 0) ? books_[symbol].bids : books_[symbol].asks;
    auto it = std::lower_bound(levels.begin(), levels.end(), price, 
        [side](const PriceLevel& lvl, int64_t p) {
            return (side == 0) ? (lvl.price > p) : (lvl.price < p);
        });
    if (it != levels.end() && it->price == price && it->count == 0) {
        levels.erase(it);
    }
}

// === Order Management ===
void MultiOrderBook::send_order(uint64_t our_id, uint16_t symbol, int side,
                                int64_t price, int64_t qty) {
    uint64_t exchange_id = venue_.send_order(our_id, symbol, side, price, qty);
    insert_our_mapping(our_id, exchange_id);
}

void MultiOrderBook::modify_our_order(uint64_t our_id, int64_t new_price, int64_t new_qty) {
    uint64_t old_exchange_id = find_our_exchange_id(our_id);
    if (old_exchange_id == 0) return;
    uint64_t new_exchange_id = venue_.modify_order(old_exchange_id, new_price, new_qty);
    if (new_exchange_id != old_exchange_id) {
        remove_our_mapping(our_id);
        insert_our_mapping(our_id, new_exchange_id);
    }
}

void MultiOrderBook::cancel_our_order(uint64_t our_id) {
    uint64_t exchange_id = find_our_exchange_id(our_id);
    if (exchange_id != 0) {
        venue_.cancel_order(exchange_id);
        remove_our_mapping(our_id);
    }
}

// === Exchange Feed ===
void MultiOrderBook::add_order(uint64_t exchange_id, uint16_t symbol, int side,
                               int64_t price, int64_t qty) {
    uint32_t order_idx = allocate_order();
    if (order_idx == 0xFFFFFFFF) return;

    OrderNode& node = order_pool_[order_idx];
    node.exchange_id = exchange_id;
    node.our_id = 0; // Linked when matched if ours
    node.symbol = symbol;
    node.side = static_cast<int8_t>(side);
    node.price = price;
    node.qty = qty;
    node.prev_idx = 0xFFFFFFFF;
    node.next_idx = 0xFFFFFFFF;

    insert_order_hash(exchange_id, order_idx);

    PriceLevel* lvl = find_or_create_level(symbol, side, price);
    lvl->total_qty += qty;
    lvl->count++;

    if (lvl->tail_order_idx == 0xFFFFFFFF) {
        lvl->head_order_idx = order_idx;
        lvl->tail_order_idx = order_idx;
    } else {
        order_pool_[lvl->tail_order_idx].next_idx = order_idx;
        node.prev_idx = lvl->tail_order_idx;
        lvl->tail_order_idx = order_idx;
    }
}

void MultiOrderBook::modify_order(uint64_t exchange_id, int64_t new_qty) {
    uint32_t order_idx = find_order(exchange_id);
    if (order_idx == 0xFFFFFFFF) return;

    OrderNode& node = order_pool_[order_idx];
    int64_t diff = node.qty - new_qty;
    node.qty = new_qty;

    PriceLevel* lvl = find_level(node.symbol, node.side, node.price);
    if (lvl) {
        lvl->total_qty -= diff;
    }
}

void MultiOrderBook::cancel_order(uint64_t exchange_id) {
    uint32_t order_idx = find_order(exchange_id);
    if (order_idx == 0xFFFFFFFF) return;

    OrderNode& node = order_pool_[order_idx];
    PriceLevel* lvl = find_level(node.symbol, node.side, node.price);
    if (lvl) {
        lvl->total_qty -= node.qty;
        lvl->count--;

        if (node.prev_idx != 0xFFFFFFFF) {
            order_pool_[node.prev_idx].next_idx = node.next_idx;
        } else {
            lvl->head_order_idx = node.next_idx;
        }

        if (node.next_idx != 0xFFFFFFFF) {
            order_pool_[node.next_idx].prev_idx = node.prev_idx;
        } else {
            lvl->tail_order_idx = node.prev_idx;
        }

        remove_level_if_empty(node.symbol, node.side, node.price);
    }

    remove_order_hash(exchange_id);
    free_order(order_idx);
}

// === Queries ===
TopLevel MultiOrderBook::best_bid(uint16_t symbol) const {
    const auto& bids = books_[symbol].bids;
    if (bids.empty()) return {0, 0, 0};
    return {bids[0].price, bids[0].total_qty, bids[0].count};
}

TopLevel MultiOrderBook::best_ask(uint16_t symbol) const {
    const auto& asks = books_[symbol].asks;
    if (asks.empty()) return {0, 0, 0};
    return {asks[0].price, asks[0].total_qty, asks[0].count};
}

int MultiOrderBook::get_top_levels(uint16_t symbol, int side, int n, TopLevel* out) const {
    const auto& levels = (side == 0) ? books_[symbol].bids : books_[symbol].asks;
    int count = std::min(n, static_cast<int>(levels.size()));
    for (int i = 0; i < count; ++i) {
        out[i] = {levels[i].price, levels[i].total_qty, levels[i].count};
    }
    return count;
}

int64_t MultiOrderBook::volume_near_best(uint16_t symbol, int side, int64_t depth) const {
    const auto& levels = (side == 0) ? books_[symbol].bids : books_[symbol].asks;
    if (levels.empty()) return 0;

    int64_t best_p = levels[0].price;
    int64_t total_qty = 0;

    for (const auto& lvl : levels) {
        int64_t dist = (side == 0) ? (best_p - lvl.price) : (lvl.price - best_p);
        if (dist >= 0 && dist < depth) {
            total_qty += lvl.total_qty;
        } else if (dist >= depth) {
            break;
        }
    }
    return total_qty;
}

QueuePosition MultiOrderBook::get_queue_position(uint64_t our_id) const {
    uint64_t exchange_id = find_our_exchange_id(our_id);
    if (exchange_id == 0) return {-1, 0};

    uint32_t order_idx = find_order(exchange_id);
    if (order_idx == 0xFFFFFFFF) return {-1, 0};

    const OrderNode& node = order_pool_[order_idx];
    const PriceLevel* lvl = const_cast<MultiOrderBook*>(this)->find_level(node.symbol, node.side, node.price);
    if (!lvl) return {-1, 0};

    int index = 0;
    int64_t qty_ahead = 0;
    uint32_t curr = lvl->head_order_idx;

    while (curr != 0xFFFFFFFF && curr != order_idx) {
        qty_ahead += order_pool_[curr].qty;
        index++;
        curr = order_pool_[curr].next_idx;
    }

    if (curr == 0xFFFFFFFF) return {-1, 0};
    return {index, qty_ahead};
}

} // namespace hftu