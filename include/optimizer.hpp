// Cost-based optimizer: chooses between a sequential scan and a B+ tree index scan
// using page-access estimates.  Costs are in "page accesses"; they are estimates,
// not measurements.
#pragma once
#include <algorithm>

#include "sql.hpp"
#include "types.hpp"

namespace adb {

struct RCond {  // condition resolved against a schema
    int col;
    Op op;
    Value v;
};

// Table statistics used for estimation.
struct TableSnapshot {
    size_t rows = 0, pages = 0;
    std::vector<Column> cols;
    std::vector<size_t> distinct;                                  // per column
    std::vector<std::optional<std::pair<double, double>>> range;  // numeric min/max per column
};

// Key range implied by the conditions on one column.
struct Bounds {
    std::optional<Value> lo, hi;
    bool lo_inc = true, hi_inc = true;
    bool eq() const { return lo && hi && lo_inc && hi_inc && compare(*lo, *hi) == 0; }
};

inline Bounds makeBounds(int col, const std::vector<RCond>& conds) {
    Bounds b;
    auto setLo = [&](const Value& v, bool inc) {
        if (!b.lo) { b.lo = v; b.lo_inc = inc; return; }
        int c = compare(v, *b.lo);
        if (c > 0 || (c == 0 && !inc)) { b.lo = v; b.lo_inc = inc; }
    };
    auto setHi = [&](const Value& v, bool inc) {
        if (!b.hi) { b.hi = v; b.hi_inc = inc; return; }
        int c = compare(v, *b.hi);
        if (c < 0 || (c == 0 && !inc)) { b.hi = v; b.hi_inc = inc; }
    };
    for (const auto& c : conds) {
        if (c.col != col) continue;
        switch (c.op) {
            case Op::EQ: setLo(c.v, true); setHi(c.v, true); break;
            case Op::GT: setLo(c.v, false); break;
            case Op::GE: setLo(c.v, true); break;
            case Op::LT: setHi(c.v, false); break;
            case Op::LE: setHi(c.v, true); break;
            case Op::NE: break;  // not index-friendly
        }
    }
    return b;
}

inline bool indexable(int col, const std::vector<RCond>& conds) {
    for (const auto& c : conds) if (c.col == col && c.op != Op::NE) return true;
    return false;
}

// Fraction of rows expected to satisfy the bounds (0..1].
inline double estimateSelectivity(const TableSnapshot& s, int col, const Bounds& b) {
    if (s.rows == 0) return 1.0;
    double floor_sel = 1.0 / static_cast<double>(s.rows);
    if (b.eq()) return std::max(floor_sel, 1.0 / static_cast<double>(std::max<size_t>(1, s.distinct[col])));
    const auto& rg = s.range[col];
    if (rg && rg->second > rg->first && (!b.lo || isNumeric(*b.lo)) && (!b.hi || isNumeric(*b.hi))) {
        double span = rg->second - rg->first;
        double lo_f = b.lo ? std::clamp((asDouble(*b.lo) - rg->first) / span, 0.0, 1.0) : 0.0;
        double hi_f = b.hi ? std::clamp((asDouble(*b.hi) - rg->first) / span, 0.0, 1.0) : 1.0;
        return std::max(floor_sel, hi_f - lo_f);
    }
    return (b.lo && b.hi) ? 0.25 : 1.0 / 3.0;  // default guesses for un-summarised ranges
}

constexpr double INDEX_FANOUT = 100.0;

inline size_t estimateHeight(size_t rows) {
    size_t h = 1;
    double cap = INDEX_FANOUT;
    while (cap < static_cast<double>(rows)) { cap *= INDEX_FANOUT; ++h; }
    return h;
}
inline double seqCost(size_t pages) { return static_cast<double>(std::max<size_t>(1, pages)); }
// root-to-leaf descent + leaf pages walked + heap pages fetched (unclustered: one per match,
// bounded by the table size).
inline double indexCost(size_t height, size_t leaves, double sel, size_t rows, size_t pages) {
    double matches = sel * static_cast<double>(rows);
    double leaf_pages = std::max(1.0, std::ceil(sel * static_cast<double>(leaves)));
    double heap_pages = std::min(matches, static_cast<double>(pages));
    return static_cast<double>(height) + leaf_pages + heap_pages;
}

struct IndexInfo {
    std::string name;
    int col;
    size_t height, leaves;
};

struct Plan {
    bool use_index = false;
    std::string index_name;
    int col = -1;
    Bounds bounds;
    double seq_cost = 0, index_cost = -1, selectivity = 1.0;
};

inline Plan choosePlan(const TableSnapshot& s, const std::vector<RCond>& conds, const std::vector<IndexInfo>& idx) {
    Plan p;
    p.seq_cost = seqCost(s.pages);
    for (const auto& ix : idx) {
        if (!indexable(ix.col, conds)) continue;
        Bounds b = makeBounds(ix.col, conds);
        double sel = estimateSelectivity(s, ix.col, b);
        double cost = indexCost(ix.height, ix.leaves, sel, s.rows, s.pages);
        if (p.index_cost < 0 || cost < p.index_cost) {
            p.index_cost = cost; p.selectivity = sel; p.col = ix.col; p.index_name = ix.name; p.bounds = b;
        }
    }
    p.use_index = p.index_cost >= 0 && p.index_cost < p.seq_cost;
    return p;
}

}  // namespace adb

