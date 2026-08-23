// Challenge 01: Order Book — Skeleton Implementation
// This is a naive reference. You can do much better!

#include "solution.h"


namespace hftu {
	// 4. The Optimized Order Book

	OrderBook::OrderBook(size_t max_orders) : arena(max_orders) {
		order_lookup.resize(max_orders + 1, nullptr);
		bids.resize(MAX_TICKS);
		asks.resize(MAX_TICKS);
	}

	void OrderBook::add_order(uint64_t id, int side, int64_t price, int64_t quantity) {
		Order* order = arena.allocate();
		order->id = id;
		order->side = side;
		order->price = price;
		order->quantity = quantity;
		order->prev = nullptr;
		order->next = nullptr;

		order_lookup[id] = order;

		if (side == SIDE_BID) {
			bids[price].push_back(order);
			// Update best bid if this price is higher
			if (price > best_bid_price) {
				best_bid_price = price;
			}
		} else {
			asks[price].push_back(order);
			// Update best ask if this price is lower
			if (price < best_ask_price) {
				best_ask_price = price;
			}
		}
	}

	void OrderBook::cancel_order(uint64_t id) {
		Order* order = order_lookup[id];
		if (!order) return; // Ignore invalid IDs

		int64_t price = order->price;
		int side = order->side;

		if (side == SIDE_BID) {
			bids[price].erase(order);

			// If the best bid level was just emptied, scan down to find the next best bid
			if (price == best_bid_price && bids[price].is_empty()) {
				while (best_bid_price > 0 && bids[best_bid_price].is_empty()) {
					best_bid_price--;
				}
			}
		} else {
			asks[price].erase(order);

			// If the best ask level was just emptied, scan up to find the next best ask
			if (price == best_ask_price && asks[price].is_empty()) {
				while (best_ask_price < MAX_TICKS - 1 && asks[best_ask_price].is_empty()) {
					best_ask_price++;
				}
			}
		}

		order_lookup[id] = nullptr;
		arena.deallocate(order);
	}

} // namespace hftu
