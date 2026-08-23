
// Challenge 01: Order Book — Skeleton Implementation
// This is a naive reference. You can do much better!
#pragma once

#include <iostream>
#include <vector>
#include <cstdint>

namespace hftu {
	// 1. The Intrusive Order Node
	struct alignas(32) Order {
		uint64_t id;
		int32_t price;
		int32_t quantity;
		int32_t side;
		uint32_t prev;
		uint32_t next;
		uint32_t padding; // Forces exactly 32 bytes
	};

	static constexpr uint32_t NULL_IDX = 0xFFFFFFFF;

	struct PriceLevel {
		uint32_t head = NULL_IDX;
		uint32_t tail = NULL_IDX;
	};

	// 4. The Optimized Order Book
	class OrderBook {
		public:
			static constexpr int SIDE_BID = 0;
			static constexpr int SIDE_ASK = 1;
			static constexpr int64_t MAX_TICKS = 10'000'000; // e.g., prices from $0.00 to $1000.00 in pennies

			OrderBook(size_t max_orders = 1'0000'000); 

			void add_order(uint64_t id, int side, int64_t price, int64_t quantity);

			void cancel_order(uint64_t id);

			// Pure O(1) lookups
			int64_t best_bid() const { return best_bid_price > 0 ? best_bid_price : 0; }
			int64_t best_ask() const { return best_ask_price < MAX_TICKS - 1 ? best_ask_price : 0; }

		private:
			std::vector<Order> arena;
			std::vector<uint32_t> order_lookup;
			uint32_t free_head;

			std::vector<PriceLevel> bids;
			std::vector<PriceLevel> asks;

			std::vector<uint64_t> bid_bitmask;
			std::vector<uint64_t> ask_bitmask;

			int32_t best_bid_price = 0;
			int32_t best_ask_price = MAX_TICKS - 1;

			// --- Intrusive List Helpers (Indices instead of pointers) ---
			void insert_into_level(PriceLevel& level, uint32_t idx) {
				Order& o = arena[idx];
				o.prev = level.tail;
				if (level.tail != NULL_IDX) arena[level.tail].next = idx;
				else level.head = idx;
				level.tail = idx;
			}

			void remove_from_level(PriceLevel& level, uint32_t idx) {
				Order& o = arena[idx];
				if (o.prev != NULL_IDX) arena[o.prev].next = o.next;
				else level.head = o.next;

				if (o.next != NULL_IDX) arena[o.next].prev = o.prev;
				else level.tail = o.prev;
			}

			// --- Hardware Bitmask Scanning ---
			void update_best_bid(int32_t current_price) {
				int32_t word_idx = current_price >> 6;
				while (word_idx >= 0) {
					uint64_t w = bid_bitmask[word_idx];
					if (w) {
						// __builtin_clzll = Count Leading Zeros.
						// 63 - clzll finds the highest set bit in 1 CPU cycle.
						best_bid_price = (word_idx << 6) + (63 - __builtin_clzll(w));
						return;
					}
					word_idx--;
				}
				best_bid_price = 0;
			}

			void update_best_ask(int32_t current_price) {
				int32_t word_idx = current_price >> 6;
				int32_t max_word = ask_bitmask.size();
				while (word_idx < max_word) {
					uint64_t w = ask_bitmask[word_idx];
					if (w) {
						// __builtin_ctzll = Count Trailing Zeros.
						// Finds the lowest set bit in 1 CPU cycle.
						best_ask_price = (word_idx << 6) + __builtin_ctzll(w);
						return;
					}
					word_idx++;
				}
				best_ask_price = MAX_TICKS - 1;
			}
	};

} // namespace hftu
