#include "solution.h"

#include <algorithm>
#include <cstdint>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace hftu {

namespace {
void advise_hugepages(void* p, size_t n) noexcept {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    if (p) madvise(p, n, MADV_HUGEPAGE);
#else
    (void)p;
    (void)n;
#endif
}
} // namespace

MultiOrderBook::MultiOrderBook(Venue& venue) : venue_(venue) {
    sides_ = new SideState[NUM_SYMBOLS * 2]();   // zeroed
    for (size_t i = 0; i < NUM_SYMBOLS * 2; ++i) {
        SideState& st = sides_[i];
        st.best_ref = NULL_REF;
        st.best_price = ((i & 1) == 0) ? INT64_MIN : INT64_MAX;
        // Directory reserves: hot sides hold ~900 far levels = ~28 chunks.
        // 256 entries = 8x headroom; growth beyond still works (slow path).
        st.far_below.reserve(256);
        st.far_above.reserve(256);
    }
    advise_hugepages(sides_, sizeof(SideState) * NUM_SYMBOLS * 2);

    nodes_.assign(NODE_DIRECT_CAP, ~0ULL);
    advise_hugepages(nodes_.data(), nodes_.size() * sizeof(uint64_t));

    our_direct_.assign(OUR_DIRECT_CAP, OurEnt{0, 0, 0});

    ring_qty_.resize(3u << 20);   // 3M units, zeroed (pre-faulted)
    ring_idx_.resize(3u << 20);
    ring_bump_ = 1;               // handle 0 is reserved as "no ring"
    for (uint32_t& h : ring_free_) h = 0;
    advise_hugepages(ring_qty_.data(), ring_qty_.size() * sizeof(int64_t));
    advise_hugepages(ring_idx_.data(), ring_idx_.size() * sizeof(uint32_t));

    chunk_pool_.reserve(8192 * CHUNK_CAP);   // 8K chunks = 256K levels headroom
    fdata_.assign(FAR_SLOT_MAX, FData{0, 0, 0, 0});
    advise_hugepages(fdata_.data(), fdata_.size() * sizeof(FData));
    far_slot_next_ = 1;

    ovf_hash_.assign(size_t{1} << 16, OvfEnt{0, 0, 0});
    ovf_count_ = 0;

    for (size_t s = 0; s < NUM_SYMBOLS; ++s) {
        base_[s] = 0;
        learn_cnt_[s] = 0;
        learned_[s] = 0;
    }
    pending_learn_ = 0;
}

MultiOrderBook::~MultiOrderBook() {
    delete[] sides_;
}

void MultiOrderBook::flush_symbol(uint16_t symbol) {
    if (learned_[symbol]) return;
    learned_[symbol] = 1;
    const uint8_t n = learn_cnt_[symbol];
    learn_cnt_[symbol] = 0;
    if (n == 0) {
        base_[symbol] = 50000;   // arbitrary; no samples seen
        return;                   // never contributed to pending_learn_
    }
    --pending_learn_;             // only symbols with buffered adds counted
    LearnEnt local[64];
    for (uint8_t i = 0; i < n; ++i) local[i] = learn_buf_[symbol][i];
    int64_t prices[64];
    for (uint8_t i = 0; i < n; ++i) prices[i] = local[i].price;
    std::sort(prices, prices + n);
    base_[symbol] = prices[n / 2];   // median: robust to far outliers
    for (uint8_t i = 0; i < n; ++i)
        add_order(local[i].eid, symbol, static_cast<int>(local[i].side_idx & 1),
                  local[i].price, local[i].qty);
}

void MultiOrderBook::flush_all() {
    for (size_t s = 0; s < NUM_SYMBOLS; ++s)
        if (learned_[s] == 0 && learn_cnt_[s] > 0) flush_symbol(static_cast<uint16_t>(s));
}

void MultiOrderBook::ring_pool_grow() {
    ring_qty_.resize(ring_qty_.size() * 2);
    ring_idx_.resize(ring_idx_.size() * 2);
    advise_hugepages(ring_qty_.data(), ring_qty_.size() * sizeof(int64_t));
    advise_hugepages(ring_idx_.data(), ring_idx_.size() * sizeof(uint32_t));
}

// ---------------------------------------------------------------------------
// Overflow node path: exchange ids >= NODE_DIRECT_CAP (never hit by the
// benchmark's sequential ids; kept so arbitrary feeds stay correct).
// ---------------------------------------------------------------------------

namespace {
uint64_t ovf_mix(uint64_t eid) noexcept {
    uint64_t k = eid * 0x9E3779B97F4A7C15ULL;
    k ^= k >> 30;
    k *= 0xBF58476D1CE4E5B9ULL;
    k ^= k >> 27;
    k *= 0x94D049BB133111EBULL;
    k ^= k >> 31;
    return k;
}
} // namespace

uint32_t MultiOrderBook::ovf_acquire(uint64_t eid) {
    uint32_t pos;
    if (!ovf_free_.empty()) {
        pos = ovf_free_.back();
        ovf_free_.pop_back();
        ovf_nodes_[pos] = ~0ULL;
    } else {
        pos = static_cast<uint32_t>(ovf_nodes_.size());
        ovf_nodes_.push_back(~0ULL);
    }
    const uint32_t idx = static_cast<uint32_t>(NODE_DIRECT_CAP) + pos;
    if ((ovf_count_ + 1) * 2 > ovf_hash_.size()) ovf_grow();
    OvfEnt* t = ovf_hash_.data();
    const uint32_t mask = static_cast<uint32_t>(ovf_hash_.size()) - 1;
    uint32_t i = static_cast<uint32_t>(ovf_mix(eid)) & mask;
    while (t[i].occ) i = (i + 1) & mask;
    t[i] = OvfEnt{eid, idx, 1};
    ++ovf_count_;
    return idx;
}

uint32_t MultiOrderBook::ovf_lookup(uint64_t eid, uint32_t& idx_out) const noexcept {
    const OvfEnt* t = ovf_hash_.data();
    const uint32_t mask = static_cast<uint32_t>(ovf_hash_.size()) - 1;
    uint32_t i = static_cast<uint32_t>(ovf_mix(eid)) & mask;
    while (t[i].occ) {
        if (t[i].eid == eid) {
            idx_out = t[i].idx;
            return static_cast<uint32_t>(ovf_nodes_[t[i].idx - NODE_DIRECT_CAP]);
        }
        i = (i + 1) & mask;
    }
    return NULL_REF;
}

void MultiOrderBook::ovf_release(uint64_t eid, uint32_t idx) noexcept {
    OvfEnt* t = ovf_hash_.data();
    const uint32_t mask = static_cast<uint32_t>(ovf_hash_.size()) - 1;
    uint32_t i = static_cast<uint32_t>(ovf_mix(eid)) & mask;
    while (t[i].occ) {
        if (t[i].eid == eid) {
            t[i].occ = 0;
            --ovf_count_;
            uint32_t j = i;
            for (uint32_t k = (j + 1) & mask;; k = (k + 1) & mask) {
                if (!t[k].occ) break;
                const uint32_t home = static_cast<uint32_t>(ovf_mix(t[k].eid)) & mask;
                if (((j - home) & mask) < ((k - home) & mask)) {
                    t[j] = t[k];
                    t[k].occ = 0;
                    j = k;
                }
            }
            break;
        }
        i = (i + 1) & mask;
    }
    ovf_nodes_[idx - NODE_DIRECT_CAP] = ~0ULL;
    ovf_free_.push_back(idx - static_cast<uint32_t>(NODE_DIRECT_CAP));
}

void MultiOrderBook::ovf_grow() {
    std::vector<OvfEnt> old = std::move(ovf_hash_);
    ovf_hash_.assign(old.size() * 2, OvfEnt{0, 0, 0});
    const uint32_t mask = static_cast<uint32_t>(ovf_hash_.size()) - 1;
    for (const OvfEnt& e : old) {
        if (!e.occ) continue;
        uint32_t i = static_cast<uint32_t>(ovf_mix(e.eid)) & mask;
        while (ovf_hash_[i].occ) i = (i + 1) & mask;
        ovf_hash_[i] = e;
    }
}

} // namespace hftu
