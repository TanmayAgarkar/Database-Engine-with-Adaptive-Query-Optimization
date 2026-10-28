// Adaptive layer: workload tracker (bounded aggregates), index advisor (cost-based,
// explainable), and global metrics.  The advisor only *recommends*; creating an index
// requires an explicit CREATE INDEX or APPLY INDEX RECOMMENDATIONS.
#pragma once
#include <functional>
#include <map>

#include "optimizer.hpp"

namespace adb {

struct GlobalMetrics {
    uint64_t statements = 0, selects = 0, cache_hits = 0;
    double select_ms = 0;
};

struct ColumnWorkload {
    uint64_t total = 0, eq = 0, range = 0;  // queries touching the column / indexable eq / indexable range
    double sel_sum = 0;                     // sum of observed selectivities (rows returned / table rows)
    uint64_t sel_n = 0;
    // execution history split by access path (feedback: did the index actually help?)
    uint64_t seq_n = 0, seq_examined = 0, idx_n = 0, idx_examined = 0;
    double seq_ms = 0, idx_ms = 0;
};

class WorkloadTracker {
public:
    using Key = std::pair<std::string, std::string>;  // (table, column)

    void recordRead(const std::string& table, const std::string& fingerprint, const std::vector<Column>& cols,
                    const std::vector<RCond>& conds, const std::string& plan_col, const QueryStats& st,
                    double observed_sel) {
        if (fingerprints_.count(fingerprint) || fingerprints_.size() < MAX_FINGERPRINTS) ++fingerprints_[fingerprint];
        std::map<int, std::pair<bool, bool>> per;  // col -> (has EQ, has range)
        for (const auto& c : conds) {
            auto& e = per[c.col];
            if (c.op == Op::EQ) e.first = true; else if (c.op != Op::NE) e.second = true;
        }
        for (const auto& [col, fl] : per) {
            const std::string& name = cols[col].name;
            auto& w = cols_[{table, name}];
            ++w.total;
            if (fl.first || fl.second) {
                fl.first ? ++w.eq : ++w.range;
                w.sel_sum += observed_sel; ++w.sel_n;
            }
            if (st.cache_hit) continue;  // a cache hit says nothing about access-path cost
            if (plan_col == name) { ++w.idx_n; w.idx_examined += st.rows_examined; w.idx_ms += st.exec_ms; }
            else { ++w.seq_n; w.seq_examined += st.rows_examined; w.seq_ms += st.exec_ms; }
        }
    }
    void recordWrite(const std::string& table, uint64_t n) { writes_[table] += n; }

    const std::map<Key, ColumnWorkload>& columns() const { return cols_; }
    uint64_t writes(const std::string& t) const { auto it = writes_.find(t); return it == writes_.end() ? 0 : it->second; }
    const std::map<std::string, uint64_t>& allWrites() const { return writes_; }
    size_t fingerprintCount() const { return fingerprints_.size(); }
    const std::map<std::string, uint64_t>& fingerprints() const { return fingerprints_; }

private:
    static constexpr size_t MAX_FINGERPRINTS = 1000;  // bounded memory
    std::map<Key, ColumnWorkload> cols_;
    std::map<std::string, uint64_t> writes_;
    std::map<std::string, uint64_t> fingerprints_;
};

struct AdvisorConfig {
    double lambda_w = 1.0;       // weight on write-maintenance cost
    double lambda_s = 0.5;       // weight on storage cost
    double pages_per_write = 2;  // est. extra page accesses to maintain an index per modified row
    uint64_t min_queries = 5;    // ignore columns with fewer indexable queries
};

struct Recommendation {
    std::string table, column;
    uint64_t queries = 0, eq = 0, range = 0, writes = 0;
    double selectivity = 0, seq_cost = 0, idx_cost = 0;
    double read_benefit = 0, write_cost = 0, storage_cost = 0, score = 0;
    bool recommended = false;
    std::string reason;
};

class IndexAdvisor {
public:
    // S(c) = B_read - lambda_w * W - lambda_s * M   (all in page accesses)
    static std::vector<Recommendation> recommend(
        const WorkloadTracker& tr, const std::function<TableSnapshot(const std::string&)>& snapshot,
        const std::function<bool(const std::string&, const std::string&)>& has_index, const AdvisorConfig& cfg = {}) {
        std::vector<Recommendation> out;
        for (const auto& [key, w] : tr.columns()) {
            const auto& [table, column] = key;
            if (has_index(table, column) || w.eq + w.range < cfg.min_queries) continue;
            TableSnapshot s = snapshot(table);
            int col = -1;
            for (size_t i = 0; i < s.cols.size(); ++i) if (s.cols[i].name == column) col = static_cast<int>(i);
            if (col < 0 || s.rows == 0) continue;
            Recommendation r;
            r.table = table; r.column = column; r.queries = w.total; r.eq = w.eq; r.range = w.range;
            r.writes = tr.writes(table);
            r.selectivity = w.sel_n ? w.sel_sum / static_cast<double>(w.sel_n) : 1.0 / static_cast<double>(std::max<size_t>(1, s.distinct[col]));
            r.selectivity = std::max(r.selectivity, 1.0 / static_cast<double>(s.rows));
            double entry_bytes = s.cols[col].type == Type::TEXT ? 28.0 : 20.0;
            size_t leaves = static_cast<size_t>(std::ceil(static_cast<double>(s.rows) * entry_bytes / (PAGE_SIZE * 0.7)));
            leaves = std::max<size_t>(1, leaves);
            r.seq_cost = seqCost(s.pages);
            r.idx_cost = indexCost(estimateHeight(s.rows), leaves, r.selectivity, s.rows, s.pages);
            r.read_benefit = static_cast<double>(w.eq + w.range) * std::max(0.0, r.seq_cost - r.idx_cost);
            r.write_cost = static_cast<double>(r.writes) * cfg.pages_per_write;
            r.storage_cost = static_cast<double>(leaves) * 1.2;
            r.score = r.read_benefit - cfg.lambda_w * r.write_cost - cfg.lambda_s * r.storage_cost;
            r.recommended = r.score > 0;
            if (r.seq_cost <= r.idx_cost) r.reason = "queries are not selective enough: an index scan would not read fewer pages than a table scan";
            else if (!r.recommended) r.reason = "write and storage overhead outweigh the estimated read savings";
            else r.reason = "repeated selective queries: indexed access is estimated to read fewer pages";
            out.push_back(r);
        }
        std::sort(out.begin(), out.end(), [](const Recommendation& a, const Recommendation& b) { return a.score > b.score; });
        return out;
    }
};

}  // namespace adb

