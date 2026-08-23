// Challenge 01: Order Book — Skeleton Implementation
// This is a naive reference. You can do much better!

#include "solution.h"

namespace hftu {

	void OrderBook::add_order(uint64_t id, int side, int64_t price, int64_t quantity) {
		Order order{id, side, price, quantity};

		if (side == SIDE_BID) {
			auto& list = bids[price];
			list.push_back(order);
			// Store iterator for O(1) cancellation
			orders[id] = {std::prev(list.end()), price, side};
		} else {
			auto& list = asks[price];
			list.push_back(order);
			orders[id] = {std::prev(list.end()), price, side};
		}
	}

	void OrderBook::cancel_order(uint64_t id) {
		auto it = orders.find(id);
		if (it == orders.end()) return; // Order not found

		auto& tracker = it->second;

		if (tracker.side == SIDE_BID) {
			auto& list = bids[tracker.price];
			list.erase(tracker.it); // O(1) removal using allocator!

			// Clean up the price level if it's empty to keep best_bid() O(1)
			if (list.empty()) {
				bids.erase(tracker.price);
			}
		} else {
			auto& list = asks[tracker.price];
			list.erase(tracker.it);

			if (list.empty()) {
				asks.erase(tracker.price);
			}
		}

		orders.erase(it);
	}



	int64_t OrderBook::best_bid() const {
		return bids.empty() ? 0 : bids.begin()->first;
	}

	// std::less ensures asks are sorted lowest-to-highest
	int64_t OrderBook::best_ask() const {
		return asks.empty() ? 0 : asks.begin()->first;
	}

} // namespace hftu
