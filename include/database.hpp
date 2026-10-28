// Database: ties parser, catalog, storage, indexes, optimizer, executor, cache,
// metrics and the adaptive advisor together behind execute(sql).
#pragma once
#include <map>
#include <memory>

#include "adaptive.hpp"
#include "bplus_tree.hpp"
#include "cache.hpp"
#include "catalog.hpp"
#include "heap.hpp"
#include "optimizer.hpp"
#include "sql.hpp"
#include "storage.hpp"

namespace adb {

struct ExecResult {
    QueryResult data;
    QueryStats stats;
    std::string message;
    bool is_select = false;
};

class Database {
public:
    struct Options {
        size_t pool_pages = 256;
        size_t cache_bytes = 1 << 20;
    };
    explicit Database(const std::string& path, Options opt);
    explicit Database(const std::string& path) : Database(path, Options()) {}
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    ExecResult execute(const std::string& sql);

    // introspection (used by tests, benchmarks and the CLI)
    const BufferManager& buffer() const { return bm_; }
    DiskManager& disk() { return disk_; }
    const ResultCache& cache() const { return cache_; }
    const WorkloadTracker& tracker() const { return tracker_; }
    const GlobalMetrics& metrics() const { return metrics_; }
    bool hasIndexOn(const std::string& table, const std::string& column) const;
    std::vector<Recommendation> recommendations();
    void flush();

private:
    struct IndexRt {
        IndexMeta meta;
        int col = -1;
        std::unique_ptr<BPlusTree> tree;
        uint64_t uses = 0;
    };
    struct TableRt {
        TableMeta meta;
        std::unique_ptr<TableHeap> heap;
        TableSnapshot snap;
        bool snap_valid = false;
        size_t mods = 0;
    };

    TableRt& getTable(const std::string& name);
    std::vector<IndexRt*> indexesOf(const std::string& table);
    void saveCatalog();
    void loadCatalog();
    void buildIndex(TableRt& t, const IndexMeta& m, bool persist);
    TableSnapshot& snapshot(TableRt& t);
    Plan planFor(TableRt& t, const std::vector<RCond>& conds);
    std::vector<RCond> resolveConds(const TableMeta& t, const std::vector<Cond>& w) const;
    std::string encodeRow(const Row& r) const;
    Row decodeRow(const TableMeta& t, const char* d, size_t n) const;
    RID insertRow(TableRt& t, const Row& row, const std::string& enc);
    void removeRow(TableRt& t, const Row& row, RID rid);
    void afterWrite(TableRt& t, uint64_t n, bool track_workload = true);
    template <class F> void runPlan(TableRt& t, const Plan& p, const std::vector<RCond>& conds, QueryStats& st, F&& on_match);
    std::string describePlan(const TableRt& t, const Plan& p, const std::vector<RCond>& conds) const;

    ExecResult doCreateTable(const Statement& st);
    ExecResult doInsert(const Statement& st);
    ExecResult doSelect(const Statement& st);
    ExecResult doUpdate(const Statement& st);
    ExecResult doDelete(const Statement& st);
    ExecResult doCreateIndex(const Statement& st);
    ExecResult doDropIndex(const Statement& st);
    ExecResult doShowTables();
    ExecResult doDescribe(const Statement& st);
    ExecResult doRecommendations(bool apply);
    ExecResult doMetrics();
    ExecResult doGenerate(const Statement& st);

    DiskManager disk_;
    BufferManager bm_;
    Catalog catalog_;
    std::map<std::string, TableRt> tables_;
    std::map<std::string, IndexRt> indexes_;
    ResultCache cache_;
    bool cache_enabled_ = true;
    WorkloadTracker tracker_;
    GlobalMetrics metrics_;
    AdvisorConfig advisor_cfg_;
};

}  // namespace adb

