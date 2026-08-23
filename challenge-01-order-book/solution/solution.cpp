// Challenge 01: Order Book — Skeleton Implementation
// This is a naive reference. You can do much better!

#include "solution.h"


namespace hftu {
	// 4. The Optimized Order Book

	OrderBook::OrderBook(size_t max_orders) {
		max_orders_ = max_orders;
		// 2. HUGE PAGES: Bypass standard allocators for the Arena
		size_t arena_bytes = align_to_huge_page(max_orders * sizeof(Order));
		arena = static_cast<Order*>(mmap(nullptr, arena_bytes, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0));
		// Fallback if OS HugePages are not configured
		if (arena == MAP_FAILED) {
			arena = static_cast<Order*>(mmap(nullptr, arena_bytes, PROT_READ | PROT_WRITE,
						MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		}
		// HUGE PAGES: Bypass allocators for the Lookup Table
		size_t lookup_bytes = align_to_huge_page(max_orders * 2 * sizeof(uint32_t));
		order_lookup = static_cast<uint32_t*>(mmap(nullptr, lookup_bytes, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0));
		if (order_lookup == MAP_FAILED) {
			order_lookup = static_cast<uint32_t*>(mmap(nullptr, lookup_bytes, PROT_READ | PROT_WRITE,
						MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		}
		// Initialize Lookup Table memory to NULL_IDX (-1)
		memset(order_lookup, 0xFF, lookup_bytes);

		// Pre-fault Arena & build free list
		for (size_t i = 0; i < max_orders; ++i) {
			arena[i].next = i + 1;
		}
		arena[max_orders - 1].next = NULL_IDX;
		free_head = 0;

		// Huge Pages for Bitmasks
		size_t num_words = (MAX_TICKS + 63) / 64;
		size_t bitmask_bytes = align_to_huge_page(num_words * sizeof(uint64_t));

		bid_bitmask = static_cast<uint64_t*>(mmap(nullptr, bitmask_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		ask_bitmask = static_cast<uint64_t*>(mmap(nullptr, bitmask_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

		// Use arrays for price levels
		bids = new PriceLevel[MAX_TICKS];
		asks = new PriceLevel[MAX_TICKS];
	}
	
	OrderBook::~OrderBook() {
		size_t arena_bytes = align_to_huge_page(max_orders_ * sizeof(Order));
		munmap(arena, arena_bytes);

		size_t lookup_bytes = align_to_huge_page(max_orders_ * 2 * sizeof(uint32_t));
		munmap(order_lookup, lookup_bytes);

		size_t num_words = (MAX_TICKS + 63) / 64;
		size_t bitmask_bytes = align_to_huge_page(num_words * sizeof(uint64_t));
		munmap(bid_bitmask, bitmask_bytes);
		munmap(ask_bitmask, bitmask_bytes);

		delete[] bids;
		delete[] asks;
	}

	void OrderBook::add_order(uint64_t id, int side, int64_t price, int64_t quantity) {
		// SAFETY: Prevent segfaults from arena exhaustion, ID bounds, and price bounds
		if (free_head == NULL_IDX) [[unlikely]] return;
		if (id >= max_orders_ * 2) [[unlikely]] return;
		if (price < 0 || price >= MAX_TICKS) [[unlikely]] return;

		uint32_t idx = free_head;
		free_head = arena[idx].next;

		// 3. PREFETCHING: Tell the CPU we are about to heavily write to this struct
		__builtin_prefetch(&arena[idx], 1, 3);

		Order& o = arena[idx];
		o.id = id;
		o.side = side;
		o.price = price;
		o.quantity = quantity;
		o.prev = NULL_IDX;
		o.next = NULL_IDX;

		order_lookup[id] = idx;

		if (side == SIDE_BID) {
			insert_into_level(bids[price], idx);
			bid_bitmask[price >> 6] |= (1ULL << (price & 63));

			// 4. BRANCHLESS MATH: Compiles to CMOV (Conditional Move)
			// Eliminates CPU pipeline flushes caused by branch mispredictions.
			best_bid_price = (price > best_bid_price) ? price : best_bid_price;
		} else {
			insert_into_level(asks[price], idx);
			ask_bitmask[price >> 6] |= (1ULL << (price & 63));

			// BRANCHLESS CMOV
			best_ask_price = (price < best_ask_price) ? price : best_ask_price;
		}
	}

	void OrderBook::cancel_order(uint64_t id) {
		// SAFETY: Prevent ID out-of-bounds access
		if (id >= max_orders_ * 2) [[unlikely]] return;

		uint32_t idx = order_lookup[id];
		if (idx == NULL_IDX) [[unlikely]] return;

		// PREFETCHING: Pull the Order struct into L1 cache immediately
		__builtin_prefetch(&arena[idx], 1, 3);

		Order& o = arena[idx];
		int32_t p = o.price;

		if (o.side == SIDE_BID) {
			remove_from_level(bids[p], idx);
			if (bids[p].head == NULL_IDX) {
				bid_bitmask[p >> 6] &= ~(1ULL << (p & 63));
				if (p == best_bid_price) update_best_bid(p);
			}
		} else {
			remove_from_level(asks[p], idx);
			if (asks[p].head == NULL_IDX) {
				ask_bitmask[p >> 6] &= ~(1ULL << (p & 63));
				if (p == best_ask_price) update_best_ask(p);
			}
		}

		order_lookup[id] = NULL_IDX;
		o.next = free_head;
		free_head = idx;
	}

} // namespace hftu
