#include "database.hpp"

#include <chrono>
#include <iomanip>
#include <random>
#include <unordered_set>

namespace adb {

namespace {

bool holds_int(const Value& v) { return std::holds_alternative<int64_t>(v); }

// Value stored into a column of the given type (NULL allowed, no lossy conversions).
Value coerceStore(const Value& v, const Column& c) {
    if (isNull(v)) return v;
    switch (c.type) {
        case Type::INT: if (holds_int(v)) return v; break;
        case Type::DOUBLE:
            if (std::holds_alternative<double>(v)) return v;
            if (holds_int(v)) return Value(static_cast<double>(std::get<int64_t>(v)));
            break;
        case Type::TEXT: if (std::holds_alternative<std::string>(v)) return v; break;
        case Type::BOOL: if (std::holds_alternative<bool>(v)) return v; break;
    }
    throw SchemaError("type mismatch for column '" + c.name + "': expected " + typeName(c.type));
}

bool holdsForType(const Value& v, Type t) {
    switch (t) {
        case Type::INT:
        case Type::DOUBLE: return isNumeric(v);
        case Type::TEXT: return std::holds_alternative<std::string>(v);
        case Type::BOOL: return std::holds_alternative<bool>(v);
    }
    return false;
}

bool matches(const Row& row, const std::vector<RCond>& conds) {
    for (const auto& c : conds) {
        const Value& a = row[c.col];
        if (isNull(a)) return false;  // NULL never satisfies a comparison
        int r = compare(a, c.v);
        bool ok = false;
        switch (c.op) {
            case Op::EQ: ok = r == 0; break;
            case Op::NE: ok = r != 0; break;
            case Op::LT: ok = r < 0; break;
            case Op::LE: ok = r <= 0; break;
            case Op::GT: ok = r > 0; break;
            case Op::GE: ok = r >= 0; break;
        }
        if (!ok) return false;
    }
    return true;
}

std::string pct(double x) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(2) << x * 100.0 << '%';
    return o.str();
}
std::string num(double x, int prec = 1) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << x;
    return o.str();
}

}  // namespace

// ---- construction / catalog persistence -----------------------------------------------

Database::Database(const std::string& path, Options opt)
    : disk_(path), bm_(disk_, opt.pool_pages), cache_(opt.cache_bytes) {
    if (disk_.numPages() == 0) {  // brand-new file: page 0 is reserved for the catalog
        PageId id;
        bm_.newPage(id);
        bm_.unpin(id, true);
    }
    loadCatalog();
}

Database::~Database() {
    try { flush(); } catch (...) {}
}

void Database::flush() { bm_.flushAll(); }

void Database::saveCatalog() {
    std::string s = catalog_.serialize();
    if (s.size() + 4 > PAGE_SIZE) throw SchemaError("catalog is full (it must fit in one page)");
    {
        PageGuard g(bm_, 0);
        uint32_t len = static_cast<uint32_t>(s.size());
        std::memset(g.data(), 0, PAGE_SIZE);
        std::memcpy(g.data(), &len, 4);
        std::memcpy(g.data() + 4, s.data(), s.size());
        g.markDirty();
    }
    bm_.flushAll();  // DDL is made durable immediately
}

void Database::loadCatalog() {
    std::string s;
    {
        PageGuard g(bm_, 0);
        uint32_t len;
        std::memcpy(&len, g.data(), 4);
        if (len > PAGE_SIZE - 4) throw StorageError("corrupt catalog page");
        s.assign(g.data() + 4, len);
    }
    catalog_.deserialize(s);
    for (const auto& [name, meta] : catalog_.tables()) {
        TableRt rt;
        rt.meta = meta;
        rt.heap = std::make_unique<TableHeap>(bm_, meta.first_page);
        tables_.emplace(name, std::move(rt));
    }
    // B+ tree nodes are held in memory; they are rebuilt from the heap when the database opens.
    for (const auto& [name, meta] : catalog_.indexes()) buildIndex(tables_.at(meta.table), meta, false);
}

Database::TableRt& Database::getTable(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) throw SchemaError("unknown table: " + name);
    return it->second;
}

std::vector<Database::IndexRt*> Database::indexesOf(const std::string& table) {
    std::vector<IndexRt*> v;
    for (auto& [n, ix] : indexes_) if (ix.meta.table == table) v.push_back(&ix);
    return v;
}

bool Database::hasIndexOn(const std::string& table, const std::string& column) const {
    for (const auto& [n, ix] : indexes_) if (ix.meta.table == table && ix.meta.column == column) return true;
    return false;
}

// Index becomes visible to the optimizer only after it has been completely built.
void Database::buildIndex(TableRt& t, const IndexMeta& m, bool persist) {
    IndexRt ir;
    ir.meta = m;
    ir.col = t.meta.colIndex(m.column);
    if (ir.col < 0) throw SchemaError("unknown column: " + m.column);
    ir.tree = std::make_unique<BPlusTree>(128);
    BPlusTree* tree = ir.tree.get();
    int col = ir.col;
    t.heap->scan([&](RID rid, const char* d, size_t n) {
        Row row = decodeRow(t.meta, d, n);
        if (!isNull(row[col])) tree->insert(row[col], rid);
    });
    if (persist) catalog_.addIndex(m);
    indexes_.emplace(m.name, std::move(ir));
    if (persist) {
        try { saveCatalog(); } catch (...) { indexes_.erase(m.name); catalog_.dropIndex(m.name); throw; }
    }
}

// ---- row encoding, statistics, planning -------------------------------------------------

std::string Database::encodeRow(const Row& r) const {
    std::string s;
    for (const auto& v : r) putValue(s, v);
    return s;
}

Row Database::decodeRow(const TableMeta& t, const char* d, size_t n) const {
    Row r;
    r.reserve(t.cols.size());
    const char* p = d;
    const char* end = d + n;
    for (size_t i = 0; i < t.cols.size(); ++i) r.push_back(getValue(p, end));
    return r;
}

std::vector<RCond> Database::resolveConds(const TableMeta& t, const std::vector<Cond>& w) const {
    std::vector<RCond> out;
    for (const auto& c : w) {
        int idx = t.colIndex(c.column);
        if (idx < 0) throw SchemaError("unknown column: " + c.column);
        if (isNull(c.value)) throw SchemaError("comparison with NULL is not supported");
        if (!holdsForType(c.value, t.cols[idx].type))
            throw SchemaError("type mismatch in predicate on '" + c.column + "': column is " + typeName(t.cols[idx].type));
        out.push_back({idx, c.op, c.value});
    }
    return out;
}

// Statistics are refreshed lazily (when missing or after many modifications); row and page
// counts are always current.
TableSnapshot& Database::snapshot(TableRt& t) {
    bool stale = !t.snap_valid || t.mods > std::max<size_t>(100, t.snap.rows / 10);
    if (stale) {
        size_t nc = t.meta.cols.size();
        std::vector<std::unordered_set<std::string>> seen(nc);
        std::vector<std::optional<std::pair<double, double>>> rg(nc);
        t.heap->scan([&](RID, const char* d, size_t n) {
            const char* p = d;
            const char* end = d + n;
            for (size_t c = 0; c < nc; ++c) {
                const char* start = p;
                Value v = getValue(p, end);
                if (isNull(v)) continue;
                seen[c].insert(std::string(start, static_cast<size_t>(p - start)));
                if (isNumeric(v)) {
                    double x = asDouble(v);
                    if (!rg[c]) rg[c] = {x, x};
                    else { rg[c]->first = std::min(rg[c]->first, x); rg[c]->second = std::max(rg[c]->second, x); }
                }
            }
        });
        t.snap.cols = t.meta.cols;
        t.snap.distinct.assign(nc, 0);
        for (size_t c = 0; c < nc; ++c) t.snap.distinct[c] = seen[c].size();
        t.snap.range = rg;
        t.snap_valid = true;
        t.mods = 0;
    }
    t.snap.rows = t.heap->numRows();
    t.snap.pages = t.heap->numPages();
    return t.snap;
}

Plan Database::planFor(TableRt& t, const std::vector<RCond>& conds) {
    const TableSnapshot& s = snapshot(t);
    std::vector<IndexInfo> infos;
    for (IndexRt* ix : indexesOf(t.meta.name)) {
        // Leaves hold posting lists, so size the index by its RID entries (~16 bytes each, 70% full pages).
        size_t by_entries = static_cast<size_t>(std::ceil(static_cast<double>(ix->tree->size()) * 16.0 / (PAGE_SIZE * 0.7)));
        infos.push_back({ix->meta.name, ix->col, ix->tree->height(), std::max(ix->tree->leafCount(), by_entries)});
    }
    return choosePlan(s, conds, infos);
}

std::string Database::describePlan(const TableRt& t, const Plan& p, const std::vector<RCond>& conds) const {
    (void)conds;
    std::ostringstream o;
    if (p.use_index) {
        o << "Plan: Index Scan using " << p.index_name << " on " << t.meta.name << "(" << t.meta.cols[p.col].name << ")\n";
    } else {
        o << "Plan: Sequential Scan on " << t.meta.name << "\n";
    }
    o << "  Table: rows=" << t.snap.rows << " pages=" << t.snap.pages << "\n";
    o << "  Estimated cost: sequential=" << num(p.seq_cost) << " pages";
    if (p.index_cost >= 0)
        o << ", index=" << num(p.index_cost) << " pages (estimated selectivity " << pct(p.selectivity) << ")";
    else
        o << ", index=n/a (no usable index)";
    o << "\n";
    return o.str();
}

template <class F>
void Database::runPlan(TableRt& t, const Plan& p, const std::vector<RCond>& conds, QueryStats& st, F&& on_match) {
    if (!p.use_index) {
        st.access_method = "sequential scan";
        t.heap->scan([&](RID rid, const char* d, size_t n) {
            Row row = decodeRow(t.meta, d, n);
            ++st.rows_examined;
            if (matches(row, conds)) on_match(rid, std::move(row));
        });
        return;
    }
    IndexRt& ix = indexes_.at(p.index_name);
    ++ix.uses;
    st.access_method = "index scan";
    st.index_used = p.index_name;
    std::vector<RID> rids = ix.tree->range(p.bounds.lo, p.bounds.lo_inc, p.bounds.hi, p.bounds.hi_inc);
    std::string rec;
    for (const RID& rid : rids) {
        if (!t.heap->get(rid, rec)) throw ExecutionError("index points to a deleted record (index inconsistency)");
        Row row = decodeRow(t.meta, rec.data(), rec.size());
        ++st.rows_examined;
        if (matches(row, conds)) on_match(rid, std::move(row));  // re-check the full predicate
    }
}

RID Database::insertRow(TableRt& t, const Row& row, const std::string& enc) {
    RID rid = t.heap->insert(enc);
    for (IndexRt* ix : indexesOf(t.meta.name))
        if (!isNull(row[ix->col])) ix->tree->insert(row[ix->col], rid);
    return rid;
}

void Database::removeRow(TableRt& t, const Row& row, RID rid) {
    for (IndexRt* ix : indexesOf(t.meta.name))
        if (!isNull(row[ix->col]) && !ix->tree->erase(row[ix->col], rid))
            throw IndexError("index " + ix->meta.name + " is missing an entry for a stored row");
    t.heap->erase(rid);
}

void Database::afterWrite(TableRt& t, uint64_t n, bool track_workload) {
    t.mods += n;
    cache_.invalidateTable(t.meta.name);  // coarse, always-correct invalidation
    if (track_workload) tracker_.recordWrite(t.meta.name, n);
}

// ---- statements -----------------------------------------------------------------------

ExecResult Database::execute(const std::string& sql) {
    Statement st = parseStatement(sql);
    ++metrics_.statements;
    switch (st.kind) {
        case Statement::CreateTable: return doCreateTable(st);
        case Statement::Insert: return doInsert(st);
        case Statement::Select: return doSelect(st);
        case Statement::Update: return doUpdate(st);
        case Statement::Delete: return doDelete(st);
        case Statement::CreateIndex: return doCreateIndex(st);
        case Statement::DropIndex: return doDropIndex(st);
        case Statement::ShowTables: return doShowTables();
        case Statement::Describe: return doDescribe(st);
        case Statement::ShowRecs: return doRecommendations(false);
        case Statement::ApplyRecs: return doRecommendations(true);
        case Statement::ShowMetrics: return doMetrics();
        case Statement::Generate: return doGenerate(st);
        case Statement::SetCache: {
            cache_enabled_ = st.flag;
            if (!st.flag) cache_.clear();
            ExecResult r;
            r.message = std::string("Result cache ") + (st.flag ? "enabled" : "disabled");
            return r;
        }
    }
    throw ExecutionError("unhandled statement");
}

ExecResult Database::doCreateTable(const Statement& st) {
    if (catalog_.hasTable(st.table)) throw SchemaError("table already exists: " + st.table);
    for (size_t i = 0; i < st.col_defs.size(); ++i)
        for (size_t j = i + 1; j < st.col_defs.size(); ++j)
            if (st.col_defs[i].name == st.col_defs[j].name) throw SchemaError("duplicate column name: " + st.col_defs[i].name);
    TableMeta m;
    m.name = st.table;
    m.cols = st.col_defs;
    m.first_page = TableHeap::create(bm_);
    catalog_.addTable(m);
    TableRt rt;
    rt.meta = m;
    rt.heap = std::make_unique<TableHeap>(bm_, m.first_page);
    tables_.emplace(m.name, std::move(rt));
    saveCatalog();
    ExecResult r;
    r.message = "Table " + m.name + " created";
    return r;
}

ExecResult Database::doInsert(const Statement& st) {
    TableRt& t = getTable(st.table);
    std::vector<Row> rows;
    std::vector<std::string> enc;
    for (const auto& in : st.rows) {  // validate everything first: all-or-nothing
        if (in.size() != t.meta.cols.size())
            throw SchemaError("expected " + std::to_string(t.meta.cols.size()) + " values, got " + std::to_string(in.size()));
        Row r;
        for (size_t i = 0; i < in.size(); ++i) r.push_back(coerceStore(in[i], t.meta.cols[i]));
        std::string e = encodeRow(r);
        if (e.size() > TableHeap::MAX_RECORD) throw StorageError("record too large for one page");
        rows.push_back(std::move(r));
        enc.push_back(std::move(e));
    }
    for (size_t i = 0; i < rows.size(); ++i) insertRow(t, rows[i], enc[i]);
    afterWrite(t, rows.size());
    ExecResult r;
    r.message = "INSERT " + std::to_string(rows.size());
    return r;
}

ExecResult Database::doSelect(const Statement& st) {
    TableRt& t = getTable(st.table);
    std::vector<int> proj;
    QueryResult qr;
    if (st.projection.empty()) {
        for (size_t i = 0; i < t.meta.cols.size(); ++i) { proj.push_back(static_cast<int>(i)); qr.columns.push_back(t.meta.cols[i].name); }
    } else {
        for (const auto& n : st.projection) {
            int i = t.meta.colIndex(n);
            if (i < 0) throw SchemaError("unknown column: " + n);
            proj.push_back(i);
            qr.columns.push_back(n);
        }
    }
    std::vector<RCond> conds = resolveConds(t.meta, st.where);
    ExecResult res;

    if (st.explain) {
        Plan p = planFor(t, conds);
        res.message = describePlan(t, p, conds);
        return res;
    }
    res.is_select = true;
    ++metrics_.selects;
    const std::string key = st.canonical(false), fingerprint = st.canonical(true);
    auto observed = [&](size_t returned) {
        return t.heap->numRows() ? static_cast<double>(returned) / static_cast<double>(t.heap->numRows()) : 0.0;
    };

    if (cache_enabled_) {
        if (const QueryResult* hit = cache_.get(key)) {
            res.data = *hit;
            res.stats.cache_hit = true;
            res.stats.access_method = "result cache";
            res.stats.rows_returned = hit->rows.size();
            ++metrics_.cache_hits;
            tracker_.recordRead(t.meta.name, fingerprint, t.meta.cols, conds, "", res.stats, observed(hit->rows.size()));
            return res;
        }
    }

    Plan plan = planFor(t, conds);
    uint64_t h0 = bm_.hits(), m0 = bm_.misses(), r0 = disk_.reads();
    auto t0 = std::chrono::steady_clock::now();
    runPlan(t, plan, conds, res.stats, [&](RID, Row&& row) {
        Row out;
        out.reserve(proj.size());
        for (int i : proj) out.push_back(row[i]);
        qr.rows.push_back(std::move(out));
    });
    auto t1 = std::chrono::steady_clock::now();
    res.stats.exec_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    res.stats.buffer_hits = bm_.hits() - h0;
    res.stats.buffer_misses = bm_.misses() - m0;
    res.stats.logical_reads = res.stats.buffer_hits + res.stats.buffer_misses;
    res.stats.physical_reads = disk_.reads() - r0;
    res.stats.rows_returned = qr.rows.size();
    metrics_.select_ms += res.stats.exec_ms;
    if (cache_enabled_) cache_.put(key, t.meta.name, qr);
    tracker_.recordRead(t.meta.name, fingerprint, t.meta.cols, conds, plan.use_index ? t.meta.cols[plan.col].name : "",
                        res.stats, observed(qr.rows.size()));
    res.data = std::move(qr);
    return res;
}

ExecResult Database::doUpdate(const Statement& st) {
    TableRt& t = getTable(st.table);
    std::vector<std::pair<int, Value>> sets;
    for (const auto& [name, v] : st.assignments) {
        int i = t.meta.colIndex(name);
        if (i < 0) throw SchemaError("unknown column: " + name);
        sets.emplace_back(i, coerceStore(v, t.meta.cols[i]));
    }
    std::vector<RCond> conds = resolveConds(t.meta, st.where);
    Plan plan = planFor(t, conds);
    QueryStats qs;
    std::vector<std::pair<RID, Row>> hits;  // collect first, then modify (no Halloween problem)
    runPlan(t, plan, conds, qs, [&](RID rid, Row&& row) { hits.emplace_back(rid, std::move(row)); });

    std::vector<Row> new_rows;
    std::vector<std::string> enc;
    for (auto& [rid, old] : hits) {
        Row nr = old;
        for (auto& [i, v] : sets) nr[i] = v;
        std::string e = encodeRow(nr);
        if (e.size() > TableHeap::MAX_RECORD) throw StorageError("updated record too large for one page");
        new_rows.push_back(std::move(nr));
        enc.push_back(std::move(e));
    }
    for (size_t k = 0; k < hits.size(); ++k) {  // relocate: new RID, indexes updated
        removeRow(t, hits[k].second, hits[k].first);
        insertRow(t, new_rows[k], enc[k]);
    }
    if (!conds.empty() && t.heap->numRows())
        tracker_.recordRead(t.meta.name, "update " + st.table + " where " + std::to_string(st.where.size()) + " conds", t.meta.cols, conds,
                            plan.use_index ? t.meta.cols[plan.col].name : "", qs,
                            static_cast<double>(hits.size()) / static_cast<double>(t.heap->numRows() + hits.size()));
    afterWrite(t, hits.size());
    ExecResult r;
    r.message = "UPDATE " + std::to_string(hits.size());
    r.stats = qs;
    return r;
}

ExecResult Database::doDelete(const Statement& st) {
    TableRt& t = getTable(st.table);
    std::vector<RCond> conds = resolveConds(t.meta, st.where);
    Plan plan = planFor(t, conds);
    QueryStats qs;
    std::vector<std::pair<RID, Row>> hits;
    runPlan(t, plan, conds, qs, [&](RID rid, Row&& row) { hits.emplace_back(rid, std::move(row)); });
    size_t before = t.heap->numRows();
    for (auto& [rid, row] : hits) removeRow(t, row, rid);
    if (!conds.empty() && before)
        tracker_.recordRead(t.meta.name, "delete " + st.table + " where " + std::to_string(st.where.size()) + " conds", t.meta.cols, conds,
                            plan.use_index ? t.meta.cols[plan.col].name : "", qs,
                            static_cast<double>(hits.size()) / static_cast<double>(before));
    afterWrite(t, hits.size());
    ExecResult r;
    r.message = "DELETE " + std::to_string(hits.size());
    r.stats = qs;
    return r;
}

ExecResult Database::doCreateIndex(const Statement& st) {
    TableRt& t = getTable(st.table);
    if (t.meta.colIndex(st.column) < 0) throw SchemaError("unknown column: " + st.column);
    if (catalog_.hasIndex(st.index_name)) throw SchemaError("index already exists: " + st.index_name);
    if (hasIndexOn(st.table, st.column)) throw SchemaError("column " + st.table + "." + st.column + " is already indexed");
    auto t0 = std::chrono::steady_clock::now();
    buildIndex(t, IndexMeta{st.index_name, st.table, st.column}, true);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const BPlusTree& tree = *indexes_.at(st.index_name).tree;
    ExecResult r;
    r.message = "Index " + st.index_name + " created on " + st.table + "(" + st.column + "): " + std::to_string(tree.size()) +
                " entries, height " + std::to_string(tree.height()) + ", " + std::to_string(tree.leafCount()) + " leaves, built in " +
                num(ms, 2) + " ms";
    return r;
}

ExecResult Database::doDropIndex(const Statement& st) {
    if (!catalog_.hasIndex(st.index_name)) throw SchemaError("unknown index: " + st.index_name);
    catalog_.dropIndex(st.index_name);
    indexes_.erase(st.index_name);
    saveCatalog();
    ExecResult r;
    r.message = "Index " + st.index_name + " dropped";
    return r;
}

ExecResult Database::doShowTables() {
    ExecResult r;
    r.is_select = true;
    r.data.columns = {"table", "columns", "rows", "pages", "indexes"};
    for (auto& [name, t] : tables_) {
        size_t nidx = indexesOf(name).size();
        r.data.rows.push_back({Value(name), Value(static_cast<int64_t>(t.meta.cols.size())), Value(static_cast<int64_t>(t.heap->numRows())),
                               Value(static_cast<int64_t>(t.heap->numPages())), Value(static_cast<int64_t>(nidx))});
    }
    r.stats.rows_returned = r.data.rows.size();
    return r;
}

ExecResult Database::doDescribe(const Statement& st) {
    TableRt& t = getTable(st.table);
    ExecResult r;
    r.is_select = true;
    r.data.columns = {"column", "type"};
    for (auto& c : t.meta.cols) r.data.rows.push_back({Value(c.name), Value(std::string(typeName(c.type)))});
    r.stats.rows_returned = r.data.rows.size();
    std::ostringstream o;
    o << "Table " << t.meta.name << ": rows=" << t.heap->numRows() << " pages=" << t.heap->numPages();
    for (IndexRt* ix : indexesOf(t.meta.name))
        o << "\n  index " << ix->meta.name << " on (" << ix->meta.column << "): " << ix->tree->size() << " entries, height "
          << ix->tree->height() << ", used " << ix->uses << " times";
    r.message = o.str();
    return r;
}

std::vector<Recommendation> Database::recommendations() {
    return IndexAdvisor::recommend(
        tracker_, [&](const std::string& tbl) { return snapshot(getTable(tbl)); },
        [&](const std::string& tbl, const std::string& col) { return hasIndexOn(tbl, col); }, advisor_cfg_);
}

ExecResult Database::doRecommendations(bool apply) {
    auto recs = recommendations();
    std::ostringstream o;
    if (recs.empty())
        o << "No candidate columns yet (a column needs at least " << advisor_cfg_.min_queries
          << " indexable queries without an existing index).";
    for (const auto& r : recs) {
        o << "========================================\n"
          << "        ADAPTIVE INDEX ADVISOR\n"
          << "========================================\n"
          << "Table: " << r.table << "\nColumn: " << r.column << "\n"
          << "Indexable queries: " << (r.eq + r.range) << " (equality " << r.eq << ", range " << r.range << ")   writes on table: " << r.writes << "\n"
          << "Observed selectivity: " << pct(r.selectivity) << "\n"
          << "Estimated cost per query: table scan " << num(r.seq_cost) << " pages vs index scan " << num(r.idx_cost) << " pages\n"
          << "Read benefit " << num(r.read_benefit) << " - " << advisor_cfg_.lambda_w << " x write cost " << num(r.write_cost) << " - "
          << advisor_cfg_.lambda_s << " x storage cost " << num(r.storage_cost) << " = score " << num(r.score) << "\n";
        if (r.recommended) o << "Recommendation: CREATE INDEX idx_" << r.table << "_" << r.column << " ON " << r.table << "(" << r.column << ");\n";
        else o << "Recommendation: do not index.\n";
        o << "Reason: " << r.reason << "\n";
    }
    if (apply) {
        size_t made = 0;
        for (const auto& r : recs) {
            if (!r.recommended) continue;
            std::string name = "idx_auto_" + r.table + "_" + r.column;
            if (catalog_.hasIndex(name) || hasIndexOn(r.table, r.column)) continue;
            buildIndex(getTable(r.table), IndexMeta{name, r.table, r.column}, true);
            o << "Created " << name << "\n";
            ++made;
        }
        if (!made) o << "No indexes were created.\n";
    }
    ExecResult res;
    res.message = o.str();
    return res;
}

ExecResult Database::doMetrics() {
    std::ostringstream o;
    uint64_t bh = bm_.hits(), bmiss = bm_.misses();
    o << "Statements: " << metrics_.statements << "   SELECTs: " << metrics_.selects << "   result-cache hits: " << metrics_.cache_hits
      << " (" << pct(metrics_.selects ? static_cast<double>(metrics_.cache_hits) / static_cast<double>(metrics_.selects) : 0.0) << ")\n";
    o << "Buffer pool: hits " << bh << ", misses " << bmiss << ", hit rate " << pct(bh + bmiss ? static_cast<double>(bh) / static_cast<double>(bh + bmiss) : 0.0)
      << "; disk reads " << disk_.reads() << ", disk writes " << disk_.writes() << "\n";
    o << "Result cache: " << cache_.entries() << " entries, " << cache_.bytesUsed() << " bytes\n";
    o << "Distinct query fingerprints tracked: " << tracker_.fingerprintCount() << "\n";
    o << "Per-column workload (avg rows examined / avg ms by access path; index history appears after an index is used):\n";
    for (const auto& [key, w] : tracker_.columns()) {
        o << "  " << key.first << "." << key.second << ": queries=" << w.total << " (eq " << w.eq << ", range " << w.range << ")";
        if (w.seq_n) o << " | scan: n=" << w.seq_n << " examined=" << num(static_cast<double>(w.seq_examined) / static_cast<double>(w.seq_n), 0)
                       << " ms=" << num(w.seq_ms / static_cast<double>(w.seq_n), 3);
        if (w.idx_n) o << " | index: n=" << w.idx_n << " examined=" << num(static_cast<double>(w.idx_examined) / static_cast<double>(w.idx_n), 0)
                       << " ms=" << num(w.idx_ms / static_cast<double>(w.idx_n), 3);
        o << "\n";
    }
    for (const auto& [tbl, n] : tracker_.allWrites()) o << "  writes on " << tbl << ": " << n << "\n";
    for (const auto& [name, ix] : indexes_)
        o << "  index " << name << " on " << ix.meta.table << "(" << ix.meta.column << "): " << ix.tree->size() << " entries, " << ix.tree->leafCount()
          << " leaves, used " << ix.uses << " times\n";
    ExecResult r;
    r.message = o.str();
    return r;
}

// Reproducible synthetic data (seed 42).  First INT column is a sequential key, other INTs are
// uniform in [0,100); the first TEXT column is unique, other TEXT columns take 200 distinct values.
ExecResult Database::doGenerate(const Statement& st) {
    TableRt& t = getTable(st.table);
    if (st.count <= 0) throw ExecutionError("GENERATE needs a positive row count");
    std::mt19937_64 rng(42);
    int first_int = -1, first_text = -1;
    for (size_t i = 0; i < t.meta.cols.size(); ++i) {
        if (first_int < 0 && t.meta.cols[i].type == Type::INT) first_int = static_cast<int>(i);
        if (first_text < 0 && t.meta.cols[i].type == Type::TEXT) first_text = static_cast<int>(i);
    }
    int64_t seq = static_cast<int64_t>(t.heap->numRows());
    for (int64_t n = 0; n < st.count; ++n) {
        ++seq;
        Row row;
        for (size_t i = 0; i < t.meta.cols.size(); ++i) {
            const Column& c = t.meta.cols[i];
            switch (c.type) {
                case Type::INT: row.push_back(Value(static_cast<int>(i) == first_int ? seq : static_cast<int64_t>(rng() % 100))); break;
                case Type::DOUBLE: row.push_back(Value(static_cast<double>(rng() % 100000) / 100.0)); break;
                case Type::TEXT:
                    row.push_back(Value(c.name + "_" + std::to_string(static_cast<int>(i) == first_text ? static_cast<uint64_t>(seq) : rng() % 200)));
                    break;
                case Type::BOOL: row.push_back(Value(static_cast<bool>(rng() & 1))); break;
            }
        }
        std::string enc = encodeRow(row);
        insertRow(t, row, enc);
    }
    afterWrite(t, static_cast<uint64_t>(st.count), false);  // bulk loads are not part of the steady-state write workload
    ExecResult r;
    r.message = "GENERATE " + std::to_string(st.count) + " rows into " + st.table;
    return r;
}

}  // namespace adb

