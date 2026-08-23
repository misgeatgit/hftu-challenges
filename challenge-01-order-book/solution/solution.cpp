// Challenge 01: Order Book — Skeleton Implementation
// This is a naive reference. You can do much better!

#include "solution.h"


namespace hftu {
	// 4. The Optimized Order Book

	OrderBook::OrderBook(size_t max_orders) : arena(max_orders) {
		// Pre-allocate the arena
		arena.resize(max_orders);

		// PRE-FAULTING: Touch every page so the OS maps physical memory NOW,
		// avoiding page faults during the trading session.
		for (size_t i = 0; i < max_orders; ++i) {
			arena[i].id = 0;
			arena[i].next = i + 1;
		}
		arena.back().next = NULL_IDX;
		free_head = 0;

		// Size this to your maximum expected Order ID
		order_lookup.resize(max_orders * 2, NULL_IDX);

		bids.resize(MAX_TICKS);
		asks.resize(MAX_TICKS);

		// Bitmasks for O(1) single-cycle price scanning
		int num_words = (MAX_TICKS + 63) / 64;
		bid_bitmask.resize(num_words, 0);
		ask_bitmask.resize(num_words, 0);
	}

	void OrderBook::add_order(uint64_t id, int side, int64_t price, int64_t quantity) {
		// Pop from free list (0 allocs)
		uint32_t idx = free_head;
		free_head = arena[idx].next;

		Order& o = arena[idx];
		o.id = id;
		o.side = side;
		o.price = price;
		o.quantity = quantity;
		o.prev = NULL_IDX;
		o.next = NULL_IDX;

		// Save to flat lookup
		if (id >= order_lookup.size()) [[unlikely]] {
			order_lookup.resize(id * 2, NULL_IDX);
		}
		order_lookup[id] = idx;

		if (side == SIDE_BID) {
			insert_into_level(bids[price], idx);
			bid_bitmask[price >> 6] |= (1ULL << (price & 63));

			if (price > best_bid_price) [[unlikely]] {
				best_bid_price = price;
			}
		} else {
			insert_into_level(asks[price], idx);
			ask_bitmask[price >> 6] |= (1ULL << (price & 63));

			if (price < best_ask_price) [[unlikely]] {
				best_ask_price = price;
			}
		}
	}

	void OrderBook::cancel_order(uint64_t id) {
		if (id >= order_lookup.size()) [[unlikely]] return;

		uint32_t idx = order_lookup[id];
		if (idx == NULL_IDX) [[unlikely]] return;

		Order& o = arena[idx];
		int32_t p = o.price;

		if (o.side == SIDE_BID) {
			remove_from_level(bids[p], idx);
			if (bids[p].head == NULL_IDX) {
				// Clear the bit
				bid_bitmask[p >> 6] &= ~(1ULL << (p & 63));
				if (p == best_bid_price) {
					update_best_bid(p); // Scan down
				}
			}
		} else {
			remove_from_level(asks[p], idx);
			if (asks[p].head == NULL_IDX) {
				// Clear the bit
				ask_bitmask[p >> 6] &= ~(1ULL << (p & 63));
				if (p == best_ask_price) {
					update_best_ask(p); // Scan up
				}
			}
		}

		// Return memory to free list
		order_lookup[id] = NULL_IDX;
		o.next = free_head;
		free_head = idx;
	}

} // namespace hftu
