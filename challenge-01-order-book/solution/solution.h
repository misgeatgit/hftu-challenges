#include <iostream>
#include <list>
#include <unordered_map>
#include <map>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------
// 1. The Slab Allocator (from previous step)
// ---------------------------------------------------------
template <typename T>
class SlabAllocator {
public:
    using value_type = T;

    SlabAllocator() = default;

    template <class U>
    constexpr SlabAllocator(const SlabAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n == 1) return static_cast<T*>(get_pool().allocate());
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (n == 1) get_pool().deallocate(p);
        else ::operator delete(p);
    }

    template <class U> bool operator==(const SlabAllocator<U>&) const noexcept { return true; }
    template <class U> bool operator!=(const SlabAllocator<U>&) const noexcept { return false; }

private:
    struct Pool {
        static constexpr std::size_t chunk_size = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
        static constexpr std::size_t blocks_per_slab = 1024;
        
        struct FreeNode { FreeNode* next; };
        FreeNode* free_list = nullptr;
        std::vector<void*> slabs;

        ~Pool() {
            for (void* slab : slabs) ::operator delete(slab);
        }

        void* allocate() {
            if (!free_list) {
                void* new_slab = ::operator new(chunk_size * blocks_per_slab);
                slabs.push_back(new_slab);
                char* memory = static_cast<char*>(new_slab);
                for (std::size_t i = 0; i < blocks_per_slab - 1; ++i) {
                    reinterpret_cast<FreeNode*>(memory + i * chunk_size)->next = 
                        reinterpret_cast<FreeNode*>(memory + (i + 1) * chunk_size);
                }
                reinterpret_cast<FreeNode*>(memory + (blocks_per_slab - 1) * chunk_size)->next = nullptr;
                free_list = reinterpret_cast<FreeNode*>(memory);
            }
            void* result = free_list;
            free_list = free_list->next;
            return result;
        }

        void deallocate(void* p) {
            FreeNode* node = static_cast<FreeNode*>(p);
            node->next = free_list;
            free_list = node;
        }
    };

    static Pool& get_pool() {
        thread_local Pool pool;
        return pool;
    }
};

// ---------------------------------------------------------
// 2. The Order Book Implementation
// ---------------------------------------------------------
namespace hftu {

struct Order {
    uint64_t id;
    int side;
    int64_t price;
    int64_t quantity;
};

// Define the custom list type
using OrderList = std::list<Order, SlabAllocator<Order>>;

class OrderBook {
public:
    static constexpr int SIDE_BID = 1; // Buy
    static constexpr int SIDE_ASK = 2; // Sell

    void add_order(uint64_t id, int side, int64_t price, int64_t quantity);

    void cancel_order(uint64_t id);

    // std::greater ensures bids are sorted highest-to-lowest
    int64_t best_bid() const;

    // std::less ensures asks are sorted lowest-to-highest
    int64_t best_ask() const;

private:
    // Tracks order locations for O(1) lookup and cancellation
    struct OrderTracker {
        OrderList::iterator it;
        int64_t price;
        int side;
    };

    std::map<int64_t, OrderList, std::greater<int64_t>> bids;
    std::map<int64_t, OrderList, std::less<int64_t>> asks;
    std::unordered_map<uint64_t, OrderTracker> orders;
};

} // namespace hftu

