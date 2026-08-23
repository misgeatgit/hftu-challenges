
// Challenge 01: Order Book — Skeleton Implementation
// This is a naive reference. You can do much better!
#pragma once

#include <iostream>
#include <vector>
#include <cstdint>

namespace hftu {
	// 1. The Intrusive Order Node
	struct Order {
		uint64_t id;
		int side;
		int64_t price;
		int64_t quantity;
		Order* prev = nullptr;
		Order* next = nullptr;
	};

	// 2. The Price Level Queue (O(1) insertion and deletion)
	struct PriceLevelQueue {
		Order* head = nullptr;
		Order* tail = nullptr;

		bool is_empty() const { return head == nullptr; }

		void push_back(Order* order) {
			order->next = nullptr;
			order->prev = tail;
			if (tail) tail->next = order;
			else head = order;
			tail = order;
		}

		void erase(Order* order) {
			if (order->prev) order->prev->next = order->next;
			else head = order->next;

			if (order->next) order->next->prev = order->prev;
			else tail = order->prev;
		}
	};

	// 3. The Pre-allocated Memory Arena
	class OrderArena {
		std::vector<Order> memory_block;
		Order* free_list_head = nullptr;
		public:
		explicit OrderArena(size_t capacity) {
			memory_block.resize(capacity);
			for (size_t i = 0; i < capacity - 1; ++i) {
				memory_block[i].next = &memory_block[i + 1];
			}
			memory_block.back().next = nullptr;
			free_list_head = &memory_block[0];
		}

		Order* allocate() {
			Order* order = free_list_head;
			free_list_head = free_list_head->next;
			return order;
		}

		void deallocate(Order* order) {
			order->next = free_list_head;
			free_list_head = order;
		}
	};

	// 4. The Optimized Order Book
	class OrderBook {
		public:
			static constexpr int SIDE_BID = 1;
			static constexpr int SIDE_ASK = 2;
			static constexpr int64_t MAX_TICKS = 10'000'000; // e.g., prices from $0.00 to $1000.00 in pennies

			OrderBook(size_t max_orders = 1'0000'000); 

			void add_order(uint64_t id, int side, int64_t price, int64_t quantity);

			void cancel_order(uint64_t id);

			// Pure O(1) lookups
			int64_t best_bid() const { return best_bid_price > 0 ? best_bid_price : 0; }
			int64_t best_ask() const { return best_ask_price < MAX_TICKS - 1 ? best_ask_price : 0; }

		private:
			OrderArena arena;
			std::vector<Order*> order_lookup;

			// Flat arrays for price levels
			std::vector<PriceLevelQueue> bids;
			std::vector<PriceLevelQueue> asks;

			// Trackers for O(1) querying
			int64_t best_bid_price = 0;
			int64_t best_ask_price = MAX_TICKS - 1;
	};

} // namespace hftu
