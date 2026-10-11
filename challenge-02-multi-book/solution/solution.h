#pragma once
// Challenge 02 — Multi-Symbol Order Book
//
// Architecture (see solution/DESIGN.md for the full rationale):
//
//  * Orders are stored in a direct-indexed array keyed by exchange id (the
//    feed assigns sequential ids). A node is one u64: {level_ref, ring} so a
//    single cache miss recovers everything needed to touch the order.
//    Ids outside the array fall back to a small open-addressed table so
//    arbitrary feeds still work.
//
//  * Price levels live in two zones per symbol side:
//      - "near": prices inside a +-512-tick band around the symbol's learned
//        mid (80% of traffic clusters there). Direct-indexed rows + a 17-word
//        occupancy bitmap in L1. O(1) find, contiguous scans for
//        volume_near_best / get_top_levels.
//      - "far": prices outside the band, kept in sorted "chunks" of 32
//        levels (contiguous entry arrays) with a small sorted header vector
//        per region (below band / above band). Each far level owns a stable
//        slot id; entries carry their slot, so inserting or removing a level
//        is a pure memmove — no node or best-ref rewrites ever. A side table
//        (fdata_) holds each level's data at a stable slot so orders reach
//        it with one indirection; chunks hold light {price, slot} entries.
//
//  * Each level owns a FIFO "ring": two parallel arrays (qty[], idx[]) with
//    tombstoned entries. Find-by-id walks touch only the 4-byte idx array;
//    queue-position walks sum the contiguous qty prefix. Rings grow with
//    compaction and shrink geometrically — node back-pointers are rewritten
//    only on the rare resize.
//
//  * Nothing allocates in the steady state. All pools are preallocated.

#include "../venue.h"
#include <cstdint>
#include <cstdio>
#include <vector>

namespace hftu {

class MultiOrderBook {
public:
    explicit MultiOrderBook(Venue& venue);
    ~MultiOrderBook();

    // === Our order management ===
    void send_order(uint64_t our_id, uint16_t symbol, int side, int64_t price, int64_t qty) {
        uint64_t eid = venue_.send_order(our_id, symbol, side, price, qty);
        our_insert(our_id, eid);
    }

    void modify_our_order(uint64_t our_id, int64_t new_price, int64_t new_qty) {
        bool valid;
        uint64_t old_eid = our_find(our_id, valid);
        if (!valid) [[unlikely]] return;
        uint64_t new_eid = venue_.modify_order(old_eid, new_price, new_qty);
        if (new_eid != old_eid) [[unlikely]] our_insert(our_id, new_eid);
    }

    void cancel_our_order(uint64_t our_id) {
        bool valid;
        uint64_t eid = our_find(our_id, valid);
        if (!valid) [[unlikely]] return;
        venue_.cancel_order(eid);
        our_erase(our_id);
    }

    // === Exchange feed ===
    void add_order(uint64_t exchange_id, uint16_t symbol, int side, int64_t price, int64_t qty) {
        const uint32_t side_idx = (static_cast<uint32_t>(symbol) << 1) |
                                  static_cast<uint32_t>(side != 0);
        if (!learned_[symbol]) [[unlikely]] {
            learn_add(exchange_id, symbol, side_idx, price, qty);
            return;
        }
        const uint32_t idx = acquire_node(exchange_id);
        SideState& st = sides_[side_idx];
        const int64_t slot64 = base_[symbol] + BAND - price;
        if (static_cast<uint64_t>(slot64) <= static_cast<uint64_t>(SLOTS - 1)) [[likely]] {
            const uint32_t slot = static_cast<uint32_t>(slot64);
            LevelRow& row = st.rows[slot];
            if (row.ring != 0) [[likely]] {
                const uint32_t h = ring_append(row.ring, idx, qty);
                row.ring = h;
                row.qty += qty;
                row.count++;
                node_store(idx, near_ref_of(side_idx, slot), h);
                return;
            }
            // New near level.
            const uint32_t h = ring_alloc(0);
            ring_qty(h, 1) = qty;
            ring_idx(h, 1) = idx;
            ring_hdr_set(h, 1, 1, 2);
            row.qty = qty;
            row.count = 1;
            row.ring = h;
            const uint32_t ref = near_ref_of(side_idx, slot);
            node_store(idx, ref, h);
            near_bm_set(st, slot);
            if (is_bid(side_idx) ? price > st.best_price : price < st.best_price) {
                st.best_ref = ref;
                st.best_price = price;
            }
            return;
        }
        far_add(st, side_idx, idx, price, qty);
    }

    void modify_order(uint64_t exchange_id, int64_t new_qty) {
        if (pending_learn_ != 0) [[unlikely]] flush_all();
        uint32_t idx;
        const uint32_t ref = lookup_node(exchange_id, idx);
        if (ref == NULL_REF) [[unlikely]] return;
        const uint32_t h = node_ring(idx);
        const uint32_t used = ring_used(h);
        if ((ref & NEAR_FLAG) != 0) {
            LevelRow& row = sides_[near_side(ref)].rows[ref & 0x7FF];
            int64_t rq = row.qty;   // hoisted: overlaps the ring walk
            for (uint32_t i = 1; i <= used; ++i) {
                if (ring_idx(h, i) == idx) {
                    rq += new_qty - static_cast<int64_t>(ring_qty(h, i));
                    ring_qty(h, i) = new_qty;
                    row.qty = rq;
                    return;
                }
            }
        } else {
            FData& d = fdata_[far_slot(ref)];
            int64_t lq = d.qty;   // hoisted: overlaps the ring walk
            for (uint32_t i = 1; i <= used; ++i) {
                if (ring_idx(h, i) == idx) {
                    lq += new_qty - static_cast<int64_t>(ring_qty(h, i));
                    ring_qty(h, i) = new_qty;
                    d.qty = lq;
                    return;
                }
            }
        }
    }

    void cancel_order(uint64_t exchange_id) {
        if (pending_learn_ != 0) [[unlikely]] flush_all();
        uint32_t idx;
        const uint32_t ref = lookup_node(exchange_id, idx);
        if (ref == NULL_REF) [[unlikely]] return;
        const uint32_t h = node_ring(idx);
        node_store(idx, NULL_REF, 0);
        release_node(exchange_id, idx);

        if ((ref & NEAR_FLAG) != 0) {
            const uint32_t side_idx = near_side(ref);
            const uint32_t slot = ref & 0x7FF;
            SideState& st = sides_[side_idx];
            LevelRow& row = st.rows[slot];
            int64_t rqty = row.qty;   // hoisted: overlaps the ring walk
            const uint32_t used = ring_used(h);
            uint32_t live = ring_live(h);
            const uint32_t cap = ring_cap(h);
            for (uint32_t i = 1; i <= used; ++i) {
                if (ring_idx(h, i) == idx) {
                    rqty -= static_cast<int64_t>(ring_qty(h, i));
                    ring_idx(h, i) = 0;   // tombstone
                    --live;
                    ring_set_live(h, used, live);
                    break;
                }
            }
            row.qty = rqty;
            row.count = static_cast<int32_t>(live);
            if (live == 0) [[unlikely]] {
                ring_free_handle(h);
                row.qty = 0;
                row.count = 0;
                row.ring = 0;
                near_bm_clear(st, slot);
                if (st.best_ref == ref) best_advance(st, side_idx, ref);
            } else if (cap >= 16 && live * 4 <= cap) {
                row.ring = ring_resize(h, (h >> 28) - 2);
            }
        } else {
            const uint32_t side_idx = far_side(ref);
            SideState& st = sides_[side_idx];
            const uint32_t slot = far_slot(ref);
            FData& d = fdata_[slot];
            int64_t lqty = d.qty;   // hoisted: overlaps the ring walk
            const uint32_t used = ring_used(h);
            uint32_t live = ring_live(h);
            const uint32_t cap = ring_cap(h);
            for (uint32_t i = 1; i <= used; ++i) {
                if (ring_idx(h, i) == idx) {
                    lqty -= static_cast<int64_t>(ring_qty(h, i));
                    ring_idx(h, i) = 0;
                    --live;
                    ring_set_live(h, used, live);
                    break;
                }
            }
            d.qty = lqty;
            d.count = static_cast<int32_t>(live);
            if (live == 0) [[unlikely]] {
                ring_free_handle(h);
                if (st.best_ref == ref) best_advance(st, side_idx, ref);
                FarLoc loc = far_locate(st, side_idx, slot);
                if (loc.e != nullptr) far_remove_level(loc, slot);
            } else if (cap >= 16 && live * 4 <= cap) {
                d.ring = ring_resize(h, (h >> 28) - 2);
            }
        }
    }

    // === Queries ===
    TopLevel best_bid(uint16_t symbol) noexcept {
        if (pending_learn_ != 0) [[unlikely]] flush_symbol(symbol);
        return top_of_side(sides_[static_cast<size_t>(symbol) << 1],
                           side_idx_of(symbol, 0));
    }

    TopLevel best_ask(uint16_t symbol) noexcept {
        if (pending_learn_ != 0) [[unlikely]] flush_symbol(symbol);
        return top_of_side(sides_[(static_cast<size_t>(symbol) << 1) | 1],
                           side_idx_of(symbol, 1));
    }

    int get_top_levels(uint16_t symbol, int side, int n, TopLevel* out) noexcept {
        if (n <= 0) [[unlikely]] return 0;
        if (pending_learn_ != 0) [[unlikely]] flush_symbol(symbol);
        const uint32_t side_idx = side_idx_of(symbol, side);
        const SideState& st = sides_[side_idx];
        uint32_t ref = st.best_ref;
        int i = 0;
        while (ref != NULL_REF && i < n) {
            if ((ref & NEAR_FLAG) != 0) {
                const uint32_t slot = ref & 0x7FF;
                const LevelRow& r = st.rows[slot];
                out[i++] = {base_[symbol] + BAND - slot, r.qty, r.count};
                ref = next_ref(st, side_idx, ref);
                continue;
            }
            // Far level: locate once, then walk entries contiguously. The
            // price hint is only valid while ref is still the best level.
            FarLocC loc = (ref == st.best_ref)
                ? far_locate_hint(st, side_idx, far_slot(ref), st.best_price)
                : far_locate(st, side_idx, far_slot(ref));
            if (loc.e == nullptr) [[unlikely]] break;
            const FEnt* base = &chunk_pool_[static_cast<size_t>(loc.chunk) * CHUNK_CAP];
            if (is_bid(side_idx)) {
                for (int p = static_cast<int>(loc.pos) - 1; p >= 0 && i < n; --p)
                    __builtin_prefetch(&fdata_[base[p].slot], 0, 3);
                for (int p = static_cast<int>(loc.pos); p >= 0 && i < n; --p) {
                    const FData& d = fdata_[base[p].slot];
                    out[i++] = {base[p].price, d.qty, d.count};
                }
                ref = next_ref(st, side_idx, far_ref_of(side_idx, base[0].slot));
            } else {
                for (uint32_t p = loc.pos + 1; p < loc.hdr->count && i < n; ++p)
                    __builtin_prefetch(&fdata_[base[p].slot], 0, 3);
                for (uint32_t p = loc.pos; p < loc.hdr->count && i < n; ++p) {
                    const FData& d = fdata_[base[p].slot];
                    out[i++] = {base[p].price, d.qty, d.count};
                }
                ref = next_ref(st, side_idx,
                               far_ref_of(side_idx, base[loc.hdr->count - 1].slot));
            }
        }
        return i;
    }

    int64_t volume_near_best(uint16_t symbol, int side, int64_t depth) noexcept {
        if (depth <= 0) [[unlikely]] return 0;
        if (pending_learn_ != 0) [[unlikely]] flush_symbol(symbol);
        const uint32_t side_idx = side_idx_of(symbol, side);
        const SideState& st = sides_[side_idx];
        const uint32_t ref = st.best_ref;
        if (ref == NULL_REF) [[unlikely]] return 0;
        const int64_t bp = st.best_price;
        int64_t total = 0;
        if ((ref & NEAR_FLAG) != 0) {
            // Fast path: scan near rows contiguously.
            const int64_t slot = ref & 0x7FF;
            if (is_bid(side_idx)) {
                int64_t s_end = slot + depth - 1;
                const bool spill = s_end > 2 * BAND;
                if (s_end > 2 * BAND) s_end = 2 * BAND;
                for (int64_t s = slot; s <= s_end; ++s) total += st.rows[s].qty;
                if (spill) {
                    const int64_t lo = bp - depth + 1;
                    for (uint32_t r = far_below_last(st); r != NULL_REF;
                         r = next_ref(st, side_idx, r)) {
                        const int64_t p = fdata_[far_slot(r)].price;
                        if (p < lo) break;
                        total += far_entry_qty(st, side_idx, r);
                    }
                }
            } else {
                int64_t s_end = slot - depth + 1;
                const bool spill = s_end < 0;
                if (s_end < 0) s_end = 0;
                for (int64_t s = slot; s >= s_end; --s) total += st.rows[s].qty;
                if (spill) {
                    const int64_t hi = bp + depth - 1;
                    for (uint32_t r = far_above_first(st); r != NULL_REF;
                         r = next_ref(st, side_idx, r)) {
                        const int64_t p = fdata_[far_slot(r)].price;
                        if (p > hi) break;
                        total += far_entry_qty(st, side_idx, r);
                    }
                }
            }
            return total;
        }
        // Best is a far level: walk the chain (may cross into the near band).
        if (is_bid(side_idx)) {
            const int64_t lo = bp - depth + 1;
            for (uint32_t r = ref; r != NULL_REF; r = next_ref(st, side_idx, r)) {
                const int64_t p = ref_price(r);
                if (p < lo) break;
                total += ((r & NEAR_FLAG) != 0) ? st.rows[r & 0x7FF].qty
                                                : far_entry_qty(st, side_idx, r);
            }
        } else {
            const int64_t hi = bp + depth - 1;
            for (uint32_t r = ref; r != NULL_REF; r = next_ref(st, side_idx, r)) {
                const int64_t p = ref_price(r);
                if (p > hi) break;
                total += ((r & NEAR_FLAG) != 0) ? st.rows[r & 0x7FF].qty
                                                : far_entry_qty(st, side_idx, r);
            }
        }
        return total;
    }

    QueuePosition get_queue_position(uint64_t our_id) noexcept {
        if (pending_learn_ != 0) [[unlikely]] flush_all();
        bool valid;
        const uint64_t eid = our_find(our_id, valid);
        if (!valid) [[unlikely]] return {-1, 0};
        uint32_t idx;
        const uint32_t ref = lookup_node(eid, idx);
        if (ref == NULL_REF) [[unlikely]] return {-1, 0};   // sent, not confirmed yet
        const uint32_t h = node_ring(idx);
        const uint32_t used = ring_used(h);
        int32_t index = 0;
        int64_t qty_ahead = 0;
        for (uint32_t i = 1; i <= used; ++i) {
            const uint32_t id = ring_idx(h, i);
            if (id == 0) continue;   // tombstone
            if (id == idx) return {index, qty_ahead};
            ++index;
            qty_ahead += static_cast<int64_t>(ring_qty(h, i));
        }
        return {-1, 0};
    }

    // Debug-only introspection for the correctness fuzzer.
    void debug_dump(uint16_t symbol) {
        std::fprintf(stderr, "sym %u: learned=%u cnt=%u base=%lld pending=%d\n",
                     symbol, learned_[symbol], learn_cnt_[symbol],
                     (long long)base_[symbol], pending_learn_);
        for (int side = 0; side < 2; ++side) {
            SideState& st = sides_[(size_t(symbol) << 1) | side];
            const uint32_t side_idx = side_idx_of(symbol, side);
            std::fprintf(stderr, "  side %d: best_ref=%08x best_price=%lld\n",
                         side, st.best_ref, (long long)st.best_price);
            std::fprintf(stderr, "    near_bm:");
            for (int w = 0; w < NEAR_BM_WORDS; ++w)
                if (st.near_bm[w])
                    std::fprintf(stderr, " w%d=%016llx", w,
                                 (unsigned long long)st.near_bm[w]);
            std::fprintf(stderr, "\n    below (desc):");
            for (uint32_t r = far_below_last(st); r != NULL_REF;
                 r = next_ref(st, side_idx, r))
                std::fprintf(stderr, " %lld", (long long)fdata_[far_slot(r)].price);
            std::fprintf(stderr, "\n    above (asc):");
            for (uint32_t r = far_above_first(st); r != NULL_REF;
                 r = next_ref(st, side_idx, r))
                std::fprintf(stderr, " %lld", (long long)fdata_[far_slot(r)].price);
            std::fprintf(stderr, "\n");
        }
    }

private:
    // ------------------------------------------------------------------
    // Constants
    // ------------------------------------------------------------------
    static constexpr int BAND = 512;                       // near band half-width (ticks)
    static constexpr int SLOTS = 2 * BAND + 1;             // 1025 near slots
    static constexpr int NEAR_BM_WORDS = (SLOTS + 63) / 64;    // 17

    static constexpr uint32_t NULL_REF = 0xFFFFFFFFu;
    static constexpr uint32_t NEAR_FLAG = 1u << 30;
    static constexpr uint32_t FAR_FLAG = 1u << 29;
    static constexpr uint32_t CHUNK_CAP = 32;              // levels per far chunk
    static constexpr uint32_t FAR_SLOT_MAX = 1u << 19;     // stable far level ids
    static constexpr size_t NODE_DIRECT_CAP = size_t{1} << 20;  // direct eids < 1M
    static constexpr size_t OUR_DIRECT_CAP = size_t{1} << 14;   // direct our_ids < 16K

    // ------------------------------------------------------------------
    // Data structures
    // ------------------------------------------------------------------
    struct LevelRow {
        int64_t qty;
        int32_t count;
        uint32_t ring;   // ring handle, 0 = level absent
    };

    struct FEnt {       // chunk directory entry (sorted by price)
        int64_t price;
        uint32_t slot;  // stable level id (index into fdata_)
        uint32_t pad;
    };
    struct FData {       // stable per-level data, addressed by slot
        int64_t price;
        int64_t qty;
        int32_t count;
        uint32_t ring;
    };

    struct FarHdr {     // per-side chunk directory entry, sorted by first_price
        int64_t first_price;
        uint32_t chunk_idx;
        uint32_t count;
    };

    struct alignas(64) SideState {
        LevelRow rows[SLOTS];              // near levels, slot = base+BAND-price
        uint64_t near_bm[NEAR_BM_WORDS];   // near occupancy bitmap
        std::vector<FarHdr> far_below;     // chunks with price < base-BAND, ascending
        std::vector<FarHdr> far_above;     // chunks with price > base+BAND, ascending
        uint32_t best_ref;
        int64_t best_price;
    };

    struct FarLoc {
        FEnt* e;
        const FarHdr* hdr;
        std::vector<FarHdr>* dir;
        uint32_t chunk;
        uint32_t pos;
        int c;          // directory index of the chunk
    };
    struct FarLocC {
        const FEnt* e;
        const FarHdr* hdr;
        const std::vector<FarHdr>* dir;
        uint32_t chunk;
        uint32_t pos;
        int c;
    };

    struct OvfEnt {
        uint64_t eid;
        uint32_t idx;
        uint32_t occ;
    };

    struct OurEnt {
        uint64_t eid;
        uint32_t valid;
        uint32_t pad;
    };

    struct LearnEnt {
        uint64_t eid;
        int64_t price;
        int64_t qty;
        uint32_t side_idx;
        uint32_t pad;
    };

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------
    Venue& venue_;
    SideState* sides_;

    int64_t base_[NUM_SYMBOLS];
    LearnEnt learn_buf_[NUM_SYMBOLS][64];
    uint8_t learn_cnt_[NUM_SYMBOLS];
    uint8_t learned_[NUM_SYMBOLS];
    int pending_learn_;

    std::vector<uint64_t> nodes_;              // packed {ref lo32, ring hi32}
    std::vector<uint64_t> ovf_nodes_;          // idx = CAP + position
    std::vector<uint32_t> ovf_free_;           // positions
    std::vector<OvfEnt> ovf_hash_;
    size_t ovf_count_;

    std::vector<OurEnt> our_direct_;
    std::vector<std::pair<uint64_t, uint64_t>> our_ovf_;

    std::vector<FEnt> chunk_pool_;             // CHUNK_CAP entries per chunk
    std::vector<uint32_t> chunk_free_;
    std::vector<FData> fdata_;                 // stable per-slot level data
    std::vector<uint32_t> far_slot_free_;
    uint32_t far_slot_next_;

    // Ring storage: parallel arrays indexed by handle offset. Offset 0 holds
    // the header {used|live<<32} in qty_ and cap in idx_; entries follow.
    std::vector<int64_t> ring_qty_;
    std::vector<uint32_t> ring_idx_;
    uint32_t ring_bump_;
    uint32_t ring_free_[12];

    // ------------------------------------------------------------------
    // Ref encoding helpers
    // ------------------------------------------------------------------
    static constexpr bool is_bid(uint32_t side_idx) noexcept { return (side_idx & 1) == 0; }
    static constexpr uint32_t side_idx_of(uint16_t symbol, int side) noexcept {
        return (static_cast<uint32_t>(symbol) << 1) | static_cast<uint32_t>(side != 0);
    }

    static constexpr uint32_t near_ref_of(uint32_t side_idx, uint32_t slot) noexcept {
        return NEAR_FLAG | (side_idx << 11) | slot;
    }
    static constexpr uint32_t near_side(uint32_t ref) noexcept { return (ref >> 11) & 0x1FF; }
    static constexpr uint32_t far_ref_of(uint32_t side_idx, uint32_t slot) noexcept {
        return FAR_FLAG | (side_idx << 20) | slot;
    }
    static constexpr uint32_t far_side(uint32_t ref) noexcept { return (ref >> 20) & 0x1FF; }
    static constexpr uint32_t far_slot(uint32_t ref) noexcept { return ref & 0x7FFFF; }

    // ------------------------------------------------------------------
    // Near bitmap helpers (17 words, L1 resident)
    // ------------------------------------------------------------------
    static void near_bm_set(SideState& st, uint32_t slot) noexcept {
        st.near_bm[slot >> 6] |= 1ULL << (slot & 63);
    }
    static void near_bm_clear(SideState& st, uint32_t slot) noexcept {
        st.near_bm[slot >> 6] &= ~(1ULL << (slot & 63));
    }
    static int near_next_ge(const SideState& st, int s) noexcept {  // smallest set > s
        if (s >= SLOTS - 1) return -1;
        int w = (s + 1) >> 6;
        uint64_t m = st.near_bm[w];
        const unsigned r = static_cast<unsigned>(s + 1) & 63;
        if (r) m &= ~((1ULL << r) - 1);
        for (;;) {
            if (m) return w * 64 + __builtin_ctzll(m);
            if (++w >= NEAR_BM_WORDS) return -1;
            m = st.near_bm[w];
        }
    }
    static int near_prev_le(const SideState& st, int s) noexcept {  // largest set < s
        if (s <= 0) return -1;
        int w = (s - 1) >> 6;
        uint64_t m = st.near_bm[w];
        const unsigned r = static_cast<unsigned>(s - 1) & 63;
        m &= (r == 63) ? ~0ULL : ((1ULL << (r + 1)) - 1);
        for (;;) {
            if (m) return w * 64 + 63 - __builtin_clzll(m);
            if (--w < 0) return -1;
            m = st.near_bm[w];
        }
    }
    static int near_min_slot(const SideState& st) noexcept {
        for (int w = 0; w < NEAR_BM_WORDS; ++w)
            if (st.near_bm[w]) return w * 64 + __builtin_ctzll(st.near_bm[w]);
        return -1;
    }
    static int near_max_slot(const SideState& st) noexcept {
        for (int w = NEAR_BM_WORDS - 1; w >= 0; --w)
            if (st.near_bm[w]) return w * 64 + 63 - __builtin_clzll(st.near_bm[w]);
        return -1;
    }

    // ------------------------------------------------------------------
    // Far chunk directory helpers
    // ------------------------------------------------------------------
    // Largest chunk index whose first_price <= p ("insertion chunk"), or -1.
    static int far_hdr_le(const std::vector<FarHdr>& v, int64_t p) noexcept {
        int lo = 0, hi = static_cast<int>(v.size());
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (v[mid].first_price <= p) lo = mid + 1;
            else hi = mid;
        }
        return lo - 1;
    }

    uint32_t side_of_state(const SideState& st) const noexcept {
        return static_cast<uint32_t>(&st - sides_);
    }

    // Ref of the highest-price far level below the band, or NULL_REF.
    uint32_t far_below_last(const SideState& st) const noexcept {
        const std::vector<FarHdr>& v = st.far_below;
        if (v.empty()) return NULL_REF;
        const uint32_t side_idx = side_of_state(st);
        const FarHdr& h = v.back();
        const FEnt& e =
            chunk_pool_[static_cast<size_t>(h.chunk_idx) * CHUNK_CAP + h.count - 1];
        return far_ref_of(side_idx, e.slot);
    }
    // Ref of the lowest-price far level above the band, or NULL_REF.
    uint32_t far_above_first(const SideState& st) const noexcept {
        const std::vector<FarHdr>& v = st.far_above;
        if (v.empty()) return NULL_REF;
        const uint32_t side_idx = side_of_state(st);
        const FarHdr& h = v.front();
        const FEnt& e = chunk_pool_[static_cast<size_t>(h.chunk_idx) * CHUNK_CAP];
        return far_ref_of(side_idx, e.slot);
    }

    // Locate the chunk entry for a stable slot (needs the level's price).
    FarLoc far_locate(SideState& st, uint32_t side_idx, uint32_t slot) noexcept {
        const int64_t price = fdata_[slot].price;
        std::vector<FarHdr>& v =
            (price < base_[side_idx >> 1] - BAND) ? st.far_below : st.far_above;
        for (int c = far_hdr_le(v, price); c >= 0; --c) {
            const uint32_t chunk = v[c].chunk_idx;
            FEnt* L = &chunk_pool_[static_cast<size_t>(chunk) * CHUNK_CAP];
            for (uint32_t i = 0; i < v[c].count; ++i)
                if (L[i].slot == slot)
                    return FarLoc{&L[i], &v[c], &v, chunk, i, c};
        }
        return FarLoc{nullptr, nullptr, nullptr, 0, 0, -1};
    }
    FarLocC far_locate(const SideState& st, uint32_t side_idx, uint32_t slot) const noexcept {
        return far_locate_hint(st, side_idx, slot, fdata_[slot].price);
    }
    // Price-hinted variant: skips the fdata_ read when the price is already
    // known (e.g. st.best_price for the walk start).
    FarLocC far_locate_hint(const SideState& st, uint32_t side_idx, uint32_t slot,
                            int64_t price) const noexcept {
        const std::vector<FarHdr>& v =
            (price < base_[side_idx >> 1] - BAND) ? st.far_below : st.far_above;
        for (int c = far_hdr_le(v, price); c >= 0; --c) {
            const uint32_t chunk = v[c].chunk_idx;
            const FEnt* L = &chunk_pool_[static_cast<size_t>(chunk) * CHUNK_CAP];
            for (uint32_t i = 0; i < v[c].count; ++i)
                if (L[i].slot == slot)
                    return FarLocC{&L[i], &v[c], &v, chunk, i, c};
        }
        return FarLocC{nullptr, nullptr, nullptr, 0, 0, -1};
    }

    // Level qty for a far ref: direct stable-slot access.
    int64_t far_entry_qty(const SideState& st, uint32_t side_idx, uint32_t ref) const noexcept {
        (void)st; (void)side_idx;
        return fdata_[far_slot(ref)].qty;
    }

    // ------------------------------------------------------------------
    // Rings (parallel qty/idx arrays)
    // ------------------------------------------------------------------
    static constexpr uint32_t ring_cap_of(uint32_t cls) noexcept { return 2u << cls; }

    uint32_t ring_off(uint32_t h) const noexcept { return h & 0x0FFFFFFFu; }
    int64_t& ring_qty(uint32_t h, uint32_t i) noexcept {
        return ring_qty_[ring_off(h) + i];
    }
    const int64_t& ring_qty(uint32_t h, uint32_t i) const noexcept {
        return ring_qty_[ring_off(h) + i];
    }
    uint32_t& ring_idx(uint32_t h, uint32_t i) noexcept {
        return ring_idx_[ring_off(h) + i];
    }
    uint32_t ring_idx(uint32_t h, uint32_t i) const noexcept {
        return ring_idx_[ring_off(h) + i];
    }
    uint32_t ring_used(uint32_t h) const noexcept {
        return static_cast<uint32_t>(static_cast<uint64_t>(ring_qty_[ring_off(h)]));
    }
    uint32_t ring_live(uint32_t h) const noexcept {
        return static_cast<uint32_t>(static_cast<uint64_t>(ring_qty_[ring_off(h)]) >> 32);
    }
    uint32_t ring_cap(uint32_t h) const noexcept { return ring_idx_[ring_off(h)]; }
    void ring_hdr_set(uint32_t h, uint32_t used, uint32_t live, uint32_t cap) noexcept {
        ring_qty_[ring_off(h)] = static_cast<int64_t>(
            static_cast<uint64_t>(used) | (static_cast<uint64_t>(live) << 32));
        ring_idx_[ring_off(h)] = cap;
    }
    void ring_set_live(uint32_t h, uint32_t used, uint32_t live) noexcept {
        ring_qty_[ring_off(h)] = static_cast<int64_t>(
            static_cast<uint64_t>(used) | (static_cast<uint64_t>(live) << 32));
    }

    uint32_t ring_alloc(uint32_t cls) {
        const uint32_t h = ring_free_[cls];
        if (h != 0) {
            ring_free_[cls] = ring_idx_[ring_off(h)];
            return h;
        }
        uint32_t off = ring_bump_;
        if (static_cast<size_t>(off) + 1 + ring_cap_of(cls) > ring_idx_.size())
            ring_pool_grow();
        ring_bump_ = off + 1 + ring_cap_of(cls);
        return (cls << 28) | off;
    }

    void ring_free_handle(uint32_t h) noexcept {
        const uint32_t cls = h >> 28;
        ring_idx_[ring_off(h)] = ring_free_[cls];
        ring_free_[cls] = h;
    }

    // Re-allocate to cls_new, dropping tombstones. Node back-pointers are
    // rewritten for the surviving entries; handles stay offset-stable across
    // pool growth. Refs never change (near slot / far slot are stable).
    uint32_t ring_resize(uint32_t h, uint32_t cls_new) {
        const uint32_t hn = ring_alloc(cls_new);
        const uint32_t off = ring_off(h), offn = ring_off(hn);
        const uint32_t used = ring_used(h);
        const uint32_t live = ring_live(h);
        uint32_t j = 1;
        for (uint32_t i = 1; i <= used; ++i) {
            const uint32_t id = ring_idx_[off + i];
            if (id != 0) {
                ring_idx_[offn + j] = id;
                ring_qty_[offn + j] = ring_qty_[off + i];
                node_set_ring(id, hn);
                ++j;
            }
        }
        ring_hdr_set(hn, j - 1, live, ring_cap_of(cls_new));
        ring_free_handle(h);
        return hn;
    }

    uint32_t ring_append(uint32_t h, uint32_t idx, int64_t qty) {
        uint32_t used = ring_used(h);
        const uint32_t live = ring_live(h);
        const uint32_t cap = ring_cap(h);
        if (used == cap) [[unlikely]] {
            h = ring_resize(h, (h >> 28) + 1);
            used = live;
        }
        ring_qty(h, 1 + used) = qty;
        ring_idx(h, 1 + used) = idx;
        ring_set_live(h, used + 1, live + 1);
        return h;
    }

    // ------------------------------------------------------------------
    // Node (order) management
    // ------------------------------------------------------------------
    uint32_t acquire_node(uint64_t eid) {
        if (eid != 0 && eid < NODE_DIRECT_CAP) return static_cast<uint32_t>(eid);
        return ovf_acquire(eid);
    }

    uint32_t node_ref(uint32_t idx) const noexcept {
        return static_cast<uint32_t>(idx < NODE_DIRECT_CAP
                                         ? nodes_[idx]
                                         : ovf_nodes_[idx - NODE_DIRECT_CAP]);
    }
    uint32_t node_ring(uint32_t idx) const noexcept {
        return static_cast<uint32_t>((idx < NODE_DIRECT_CAP
                                          ? nodes_[idx]
                                          : ovf_nodes_[idx - NODE_DIRECT_CAP]) >> 32);
    }
    void node_store(uint32_t idx, uint32_t ref, uint32_t ring) noexcept {
        const uint64_t v = static_cast<uint64_t>(ref) | (static_cast<uint64_t>(ring) << 32);
        if (idx < NODE_DIRECT_CAP) nodes_[idx] = v;
        else ovf_nodes_[idx - NODE_DIRECT_CAP] = v;
    }
    void node_set_ring(uint32_t idx, uint32_t ring) noexcept {
        if (idx < NODE_DIRECT_CAP) {
            const uint64_t v = nodes_[idx];
            nodes_[idx] = (v & 0xFFFFFFFFull) | (static_cast<uint64_t>(ring) << 32);
        } else {
            const size_t p = idx - NODE_DIRECT_CAP;
            const uint64_t v = ovf_nodes_[p];
            ovf_nodes_[p] = (v & 0xFFFFFFFFull) | (static_cast<uint64_t>(ring) << 32);
        }
    }

    uint32_t lookup_node(uint64_t eid, uint32_t& idx_out) const noexcept {
        if (eid != 0 && eid < NODE_DIRECT_CAP) {
            const uint32_t r = static_cast<uint32_t>(nodes_[eid]);
            if (r == NULL_REF) return NULL_REF;
            idx_out = static_cast<uint32_t>(eid);
            return r;
        }
        return ovf_lookup(eid, idx_out);
    }

    void release_node(uint64_t eid, uint32_t idx) noexcept {
        if (idx >= NODE_DIRECT_CAP) ovf_release(eid, idx);
    }

    uint32_t ovf_acquire(uint64_t eid);                       // cold path
    uint32_t ovf_lookup(uint64_t eid, uint32_t& idx_out) const noexcept;
    void ovf_release(uint64_t eid, uint32_t idx) noexcept;
    void ovf_insert(uint64_t eid, uint32_t idx);
    void ovf_grow();

    // ------------------------------------------------------------------
    // Level access helpers
    // ------------------------------------------------------------------
    int64_t ref_price(uint32_t ref) const noexcept {
        if ((ref & NEAR_FLAG) != 0) {
            const uint32_t side_idx = near_side(ref);
            return base_[side_idx >> 1] + BAND - static_cast<int64_t>(ref & 0x7FF);
        }
        return fdata_[far_slot(ref)].price;
    }

    TopLevel top_of_side(const SideState& st, uint32_t side_idx) const noexcept {
        const uint32_t ref = st.best_ref;
        if (ref == NULL_REF) [[unlikely]] return {0, 0, 0};
        if ((ref & NEAR_FLAG) != 0) {
            const uint32_t slot = ref & 0x7FF;
            const LevelRow& r = st.rows[slot];
            return {base_[side_idx >> 1] + BAND - slot, r.qty, r.count};
        }
        const FData& d = fdata_[far_slot(ref)];
        return {d.price, d.qty, d.count};
    }

    // Next level in worse-price direction (bids: lower price, asks: higher).
    uint32_t next_ref(const SideState& st, uint32_t side_idx, uint32_t ref) const noexcept {
        if ((ref & NEAR_FLAG) != 0) {
            const uint32_t slot = ref & 0x7FF;
            if (is_bid(side_idx)) {
                const int s2 = near_next_ge(st, static_cast<int>(slot));
                if (s2 >= 0) return near_ref_of(side_idx, static_cast<uint32_t>(s2));
                return far_below_last(st);
            }
            const int s2 = near_prev_le(st, static_cast<int>(slot));
            if (s2 >= 0) return near_ref_of(side_idx, static_cast<uint32_t>(s2));
            return far_above_first(st);
        }
        const FarLocC loc = far_locate(st, side_idx, far_slot(ref));
        if (loc.e == nullptr) [[unlikely]] return NULL_REF;
        const std::vector<FarHdr>& v = *loc.dir;
        const bool below_region = fdata_[far_slot(ref)].price < base_[side_idx >> 1] - BAND;
        if (is_bid(side_idx)) {
            // Descending toward worse bids.
            if (loc.pos > 0) return far_ref_of(side_idx, (loc.e - 1)->slot);
            if (loc.c > 0) {
                const FarHdr& ph = v[loc.c - 1];
                const FEnt& pe =
                    chunk_pool_[static_cast<size_t>(ph.chunk_idx) * CHUNK_CAP + ph.count - 1];
                return far_ref_of(side_idx, pe.slot);
            }
            // Reached the region's lowest price.
            if (below_region) return NULL_REF;
            const int s = near_min_slot(st);
            if (s >= 0) return near_ref_of(side_idx, static_cast<uint32_t>(s));
            return far_below_last(st);
        }
        // Ascending toward worse asks.
        if (loc.pos + 1 < loc.hdr->count)
            return far_ref_of(side_idx, (loc.e + 1)->slot);
        if (static_cast<size_t>(loc.c) + 1 < v.size()) {
            const FEnt& ne =
                chunk_pool_[static_cast<size_t>(v[loc.c + 1].chunk_idx) * CHUNK_CAP];
            return far_ref_of(side_idx, ne.slot);
        }
        // Reached the region's highest price.
        if (!below_region) return NULL_REF;
        const int s = near_max_slot(st);
        if (s >= 0) return near_ref_of(side_idx, static_cast<uint32_t>(s));
        return far_above_first(st);
    }

    void best_advance(SideState& st, uint32_t side_idx, uint32_t removed_ref) noexcept {
        const uint32_t nr = next_ref(st, side_idx, removed_ref);
        st.best_ref = nr;
        st.best_price = (nr == NULL_REF)
            ? (is_bid(side_idx) ? INT64_MIN : INT64_MAX)
            : ref_price(nr);
    }

    // ------------------------------------------------------------------
    // Far levels (chunked, stable slot ids)
    // ------------------------------------------------------------------
    uint32_t chunk_alloc() {
        if (!chunk_free_.empty()) [[unlikely]] {
            const uint32_t c = chunk_free_.back();
            chunk_free_.pop_back();
            return c;
        }
        const uint32_t c = static_cast<uint32_t>(chunk_pool_.size() / CHUNK_CAP);
        chunk_pool_.resize(chunk_pool_.size() + CHUNK_CAP);
        return c;
    }
    void chunk_free_id(uint32_t c) noexcept { chunk_free_.push_back(c); }

    uint32_t far_slot_alloc() {
        if (!far_slot_free_.empty()) [[unlikely]] {
            const uint32_t s = far_slot_free_.back();
            far_slot_free_.pop_back();
            return s;
        }
        return far_slot_next_++;
    }

    // Find the far entry with exactly `price` on this side, or nullptr.
    FEnt* far_find_price(SideState& st, uint32_t side_idx, int64_t price) noexcept {
        const std::vector<FarHdr>& v =
            (price < base_[side_idx >> 1] - BAND) ? st.far_below : st.far_above;
        const int c = far_hdr_le(v, price);
        if (c < 0) return nullptr;
        FEnt* L = &chunk_pool_[static_cast<size_t>(v[c].chunk_idx) * CHUNK_CAP];
        const uint32_t n = v[c].count;
        for (uint32_t i = 0; i < n; ++i)
            if (L[i].price == price) [[likely]] return &L[i];
        return nullptr;
    }

    void far_add(SideState& st, uint32_t side_idx, uint32_t idx, int64_t price, int64_t qty) {
        FEnt* e = far_find_price(st, side_idx, price);
        if (e != nullptr) [[likely]] {
            FData& d = fdata_[e->slot];
            d.ring = ring_append(d.ring, idx, qty);
            d.qty += qty;
            d.count++;
            node_store(idx, far_ref_of(side_idx, e->slot), d.ring);
            return;
        }
        far_insert_level(st, side_idx, idx, price, qty);
    }

    // Create a far level at `price` (must not exist) and link the order.
    void far_insert_level(SideState& st, uint32_t side_idx, uint32_t idx, int64_t price,
                          int64_t qty) {
        std::vector<FarHdr>& v =
            (price < base_[side_idx >> 1] - BAND) ? st.far_below : st.far_above;
        int c = far_hdr_le(v, price);

        if (c >= 0 && v[c].count == CHUNK_CAP) {
            // Split the full chunk, then retry (a slot now fits).
            const uint32_t old_chunk = v[c].chunk_idx;
            FEnt* L = &chunk_pool_[static_cast<size_t>(old_chunk) * CHUNK_CAP];
            const uint32_t new_chunk = chunk_alloc();
            FEnt* NL = &chunk_pool_[static_cast<size_t>(new_chunk) * CHUNK_CAP];
            const uint32_t keep = CHUNK_CAP / 2;
            const uint32_t move = CHUNK_CAP - keep;
            for (uint32_t i = 0; i < move; ++i) NL[i] = L[keep + i];   // pure memmove
            v[c].count = keep;
            v.insert(v.begin() + c + 1, FarHdr{NL[0].price, new_chunk, move});
            far_insert_level(st, side_idx, idx, price, qty);
            return;
        }

        const uint32_t h = ring_alloc(0);
        ring_qty(h, 1) = qty;
        ring_idx(h, 1) = idx;
        ring_hdr_set(h, 1, 1, 2);
        const uint32_t slot = far_slot_alloc();
        fdata_[slot] = FData{price, qty, 1, h};
        const uint32_t ref = far_ref_of(side_idx, slot);

        if (c < 0) {
            // New lowest price: start a fresh chunk.
            const uint32_t chunk = chunk_alloc();
            FEnt* L = &chunk_pool_[static_cast<size_t>(chunk) * CHUNK_CAP];
            L[0] = FEnt{price, slot, 0};
            v.insert(v.begin(), FarHdr{price, chunk, 1});
        } else {
            const uint32_t chunk = v[c].chunk_idx;
            FEnt* L = &chunk_pool_[static_cast<size_t>(chunk) * CHUNK_CAP];
            uint32_t p = 0;
            while (p < v[c].count && L[p].price < price) ++p;
            const uint32_t n = v[c].count;
            for (uint32_t i = n; i > p; --i) L[i] = L[i - 1];   // pure memmove
            L[p] = FEnt{price, slot, 0};
            v[c].count = n + 1;
        }
        node_store(idx, ref, h);
        if (is_bid(side_idx) ? price > st.best_price : price < st.best_price) {
            st.best_ref = ref;
            st.best_price = price;
        }
    }

    // Remove the (empty) far level at loc from its chunk; free its slot.
    // Level movement is a pure memmove — refs are stable.
    void far_remove_level(const FarLoc& loc, uint32_t slot) {
        std::vector<FarHdr>& v = *loc.dir;
        FEnt* L = &chunk_pool_[static_cast<size_t>(loc.chunk) * CHUNK_CAP];
        const uint32_t n = v[loc.c].count;
        for (uint32_t i = loc.pos; i + 1 < n; ++i) L[i] = L[i + 1];   // pure memmove
        v[loc.c].count = n - 1;
        if (n - 1 == 0) {
            v.erase(v.begin() + loc.c);
            chunk_free_id(loc.chunk);
        } else if (loc.pos == 0) {
            v[loc.c].first_price = L[0].price;   // first entry was removed
        }
        far_slot_free_.push_back(slot);
    }

    // ------------------------------------------------------------------
    // Our-order id mapping
    // ------------------------------------------------------------------
    uint64_t our_find(uint64_t our_id, bool& valid) const noexcept {
        if (our_id < OUR_DIRECT_CAP) {
            const OurEnt& e = our_direct_[our_id];
            valid = e.valid != 0;
            return e.eid;
        }
        for (const auto& p : our_ovf_) {
            if (p.first == our_id) {
                valid = true;
                return p.second;
            }
        }
        valid = false;
        return 0;
    }
    void our_insert(uint64_t our_id, uint64_t eid) {
        if (our_id < OUR_DIRECT_CAP) {
            OurEnt& e = our_direct_[our_id];
            e.eid = eid;
            e.valid = 1;
            return;
        }
        for (auto& p : our_ovf_) {
            if (p.first == our_id) {
                p.second = eid;
                return;
            }
        }
        our_ovf_.emplace_back(our_id, eid);
    }
    void our_erase(uint64_t our_id) noexcept {
        if (our_id < OUR_DIRECT_CAP) {
            our_direct_[our_id].valid = 0;
            return;
        }
        for (auto& p : our_ovf_) {
            if (p.first == our_id) {
                p.second = 0;
                return;
            }
        }
    }

    // ------------------------------------------------------------------
    // Band learning
    // ------------------------------------------------------------------
    void learn_add(uint64_t eid, uint16_t symbol, uint32_t side_idx, int64_t price,
                   int64_t qty) {
        uint8_t& n = learn_cnt_[symbol];
        if (n == 0) ++pending_learn_;
        learn_buf_[symbol][n] = {eid, price, qty, side_idx, 0};
        if (++n == 64) flush_symbol(symbol);
    }

    void flush_symbol(uint16_t symbol);
    void flush_all();
    void ring_pool_grow();
};

} // namespace hftu
