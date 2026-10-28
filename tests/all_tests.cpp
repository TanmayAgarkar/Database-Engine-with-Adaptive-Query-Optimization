// Self-contained test runner (no external framework).
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <random>

#include "database.hpp"

using namespace adb;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) { ++g_fail; std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; } \
    } while (0)
#define CHECK_THROWS(expr, ExType)                                                    \
    do {                                                                              \
        ++g_checks;                                                                   \
        bool ok_ = false;                                                             \
        try { expr; } catch (const ExType&) { ok_ = true; } catch (...) {}            \
        if (!ok_) { ++g_fail; std::cout << "  FAIL " << __FILE__ << ":" << __LINE__ << "  expected " #ExType "\n"; } \
    } while (0)

static std::string tmpPath(const char* n) {
    std::string p = std::string("adb_test_") + n + ".db";
    std::remove(p.c_str());
    return p;
}

static std::vector<std::string> sorted(const ExecResult& r) {
    std::vector<std::string> v;
    for (auto& row : r.data.rows) { std::string s; for (auto& x : row) s += formatValue(x) + "|"; v.push_back(s); }
    std::sort(v.begin(), v.end());
    return v;
}

static void test_disk_persistence() {
    std::string p = tmpPath("disk");
    std::vector<char> buf(PAGE_SIZE, 0), out(PAGE_SIZE, 0);
    for (size_t i = 0; i < PAGE_SIZE; ++i) buf[i] = static_cast<char>(i * 7);
    {
        DiskManager d(p);
        CHECK(d.allocatePage() == 0);
        PageId id = d.allocatePage();
        CHECK(id == 1);
        d.writePage(1, buf.data());
    }
    DiskManager d(p);
    CHECK(d.numPages() == 2);
    d.readPage(1, out.data());
    CHECK(out == buf);
    CHECK_THROWS(d.readPage(9, out.data()), StorageError);
}

static void test_buffer_manager() {
    std::string p = tmpPath("buf");
    DiskManager d(p);
    BufferManager bm(d, 3);
    std::vector<PageId> ids;
    for (int i = 0; i < 6; ++i) {  // 6 pages through a 3-frame pool forces dirty eviction
        PageId id; char* pg = bm.newPage(id);
        pg[0] = static_cast<char>('A' + i);
        bm.unpin(id, true);
        ids.push_back(id);
    }
    for (int i = 0; i < 6; ++i) { char* pg = bm.fetch(ids[i]); CHECK(pg[0] == 'A' + i); bm.unpin(ids[i], false); }
    // LRU: touch page 3,4,5 then fetch page 0 -> page 3 should be evicted, 4/5 stay resident
    uint64_t m0 = bm.misses();
    for (int i : {3, 4, 5}) { bm.fetch(ids[i]); bm.unpin(ids[i], false); }
    CHECK(bm.misses() == m0);
    // pinned pages cannot be evicted
    bm.fetch(ids[0]); bm.fetch(ids[1]); bm.fetch(ids[2]);
    CHECK_THROWS(bm.fetch(ids[3]), StorageError);
    for (int i : {0, 1, 2}) bm.unpin(ids[i], false);
}

static void test_heap() {
    std::string p = tmpPath("heap");
    DiskManager d(p);
    BufferManager bm(d, 8);
    PageId first = TableHeap::create(bm);
    TableHeap h(bm, first);
    std::vector<RID> rids;
    for (int i = 0; i < 2000; ++i) rids.push_back(h.insert("record-" + std::to_string(i) + std::string(50, 'x')));
    CHECK(h.numRows() == 2000);
    std::string s;
    CHECK(h.get(rids[1234], s) && s.rfind("record-1234", 0) == 0);
    for (int i = 0; i < 2000; i += 2) CHECK(h.erase(rids[i]));
    CHECK(!h.get(rids[10], s));
    size_t pages_before = h.numPages();
    for (int i = 0; i < 1000; ++i) h.insert("again-" + std::to_string(i) + std::string(50, 'y'));  // reuses freed space
    CHECK(h.numPages() <= pages_before + 1);
    size_t count = 0;
    h.scan([&](RID, const char*, size_t) { ++count; });
    CHECK(count == 2000);
    CHECK_THROWS(h.insert(std::string(5000, 'z')), StorageError);
}

static void test_btree() {
    BPlusTree t(4);  // tiny nodes -> many splits
    std::multimap<int64_t, RID> ref;
    std::mt19937 rng(1);
    for (uint32_t i = 0; i < 5000; ++i) {
        int64_t k = static_cast<int64_t>(rng() % 700);
        RID r{i, 0};
        t.insert(Value(k), r);
        ref.emplace(k, r);
    }
    CHECK(t.checkInvariants());
    CHECK(t.height() > 2);
    for (int64_t k = 0; k < 720; k += 7) {
        auto got = t.search(Value(k));
        CHECK(got.size() == ref.count(k));
    }
    auto rg = t.range(Value(int64_t(100)), false, Value(int64_t(200)), true);  // (100, 200]
    size_t expect = 0;
    for (auto& kv : ref) if (kv.first > 100 && kv.first <= 200) ++expect;
    CHECK(rg.size() == expect);
    CHECK(t.range(std::nullopt, true, Value(int64_t(-1)), true).empty());
    // erase half
    size_t n = 0;
    for (auto it = ref.begin(); it != ref.end();) {
        if (n++ % 2 == 0) { CHECK(t.erase(Value(it->first), it->second)); it = ref.erase(it); } else ++it;
    }
    CHECK(t.checkInvariants());
    CHECK(t.size() == ref.size());
    CHECK(!t.erase(Value(int64_t(5)), RID{999999, 9}));
    for (int64_t k = 0; k < 700; k += 13) CHECK(t.search(Value(k)).size() == ref.count(k));
    // text keys
    BPlusTree ts(4);
    for (uint32_t i = 0; i < 300; ++i) ts.insert(Value(std::string("k") + std::to_string(i % 50)), RID{i, 0});
    CHECK(ts.search(Value(std::string("k7"))).size() == 6);
    CHECK_THROWS(ts.insert(Value(), RID{0, 0}), IndexError);
}

static void test_parser() {
    auto s = parseStatement("SELECT name, age FROM Customer WHERE age >= 20 AND city = 'Pune City';");
    CHECK(s.kind == Statement::Select && s.table == "customer" && s.projection.size() == 2 && s.where.size() == 2);
    CHECK(s.where[0].op == Op::GE && std::get<std::string>(s.where[1].value) == "Pune City");
    CHECK(s.canonical(true) == "select name,age from customer where age >= ? and city = ?");
    auto i = parseStatement("insert into t values (1, 'it''s', -2.5, true, null), (2, 'x', 0.5, false, null)");
    CHECK(i.rows.size() == 2 && std::get<std::string>(i.rows[0][1]) == "it's" && std::get<double>(i.rows[0][2]) == -2.5);
    CHECK(parseStatement("EXPLAIN SELECT * FROM t").explain);
    CHECK(parseStatement("create index i on t(c)").kind == Statement::CreateIndex);
    CHECK_THROWS(parseStatement("SELECT * FROM"), ParseError);
    CHECK_THROWS(parseStatement("SELECT * FROM t WHERE a = 'oops"), ParseError);
    CHECK_THROWS(parseStatement("SELECT * FROM t WHERE a = 1 OR b = 2"), ParseError);
    CHECK_THROWS(parseStatement("FROBNICATE"), ParseError);
    CHECK_THROWS(parseStatement("SELECT * FROM t garbage"), ParseError);
}

static void test_errors_leave_db_valid() {
    Database db(tmpPath("err"));
    db.execute("CREATE TABLE t (id INT, name TEXT)");
    CHECK_THROWS(db.execute("SELECT * FROM nope"), SchemaError);
    CHECK_THROWS(db.execute("SELECT zzz FROM t"), SchemaError);
    CHECK_THROWS(db.execute("SELECT * FROM t WHERE id = 'x'"), SchemaError);
    CHECK_THROWS(db.execute("INSERT INTO t VALUES (1)"), SchemaError);
    CHECK_THROWS(db.execute("INSERT INTO t VALUES (1, 'a'), ('bad', 'b')"), SchemaError);  // all-or-nothing
    CHECK_THROWS(db.execute("CREATE TABLE t (a INT)"), SchemaError);
    CHECK_THROWS(db.execute("CREATE INDEX i ON t(zzz)"), SchemaError);
    CHECK(db.execute("SELECT * FROM t").data.rows.empty());
    db.execute("INSERT INTO t VALUES (1, 'a')");
    CHECK(db.execute("SELECT * FROM t").data.rows.size() == 1);
}

// The key invariant: sequential scan and index scan must return the same logical result.
static void test_seq_vs_index_equivalence() {
    Database db(tmpPath("equiv"));
    db.execute("SET CACHE OFF");
    db.execute("CREATE TABLE t (id INT, v INT, w DOUBLE, s TEXT)");
    std::mt19937 rng(7);
    for (int b = 0; b < 200; ++b) {  // 20000 rows in batches of 100
        std::string sql = "INSERT INTO t VALUES ";
        for (int j = 0; j < 100; ++j) {
            int i = b * 100 + j;
            sql += std::string(j ? "," : "") + "(" + std::to_string(i) + "," +
                   (i % 97 == 0 ? std::string("NULL") : std::to_string(static_cast<int>(rng() % 1000))) + "," +
                   std::to_string(static_cast<double>(rng() % 5000) / 10.0) + ",'s" + std::to_string(rng() % 400) + "')";
        }
        db.execute(sql);
    }
    // some churn before indexing
    db.execute("DELETE FROM t WHERE id < 100");
    db.execute("UPDATE t SET v = 999 WHERE id >= 200 AND id < 230");

    std::vector<std::string> queries = {
        "SELECT * FROM t WHERE v = 10",      "SELECT * FROM t WHERE v > 990",  "SELECT * FROM t WHERE v >= 20 AND v < 25",
        "SELECT * FROM t WHERE v = 999",     "SELECT * FROM t WHERE v < 0",   "SELECT * FROM t WHERE v = 10 AND w > 25",
        "SELECT id FROM t WHERE s = 's7'",   "SELECT * FROM t WHERE s >= 's300' AND s <= 's301'",
        "SELECT * FROM t WHERE w > 12.5 AND w <= 13.5", "SELECT * FROM t WHERE id = 15000", "SELECT * FROM t WHERE v != 10"};
    std::vector<std::vector<std::string>> base;
    for (auto& q : queries) { auto r = db.execute(q); CHECK(r.stats.index_used.empty()); base.push_back(sorted(r)); }

    db.execute("CREATE INDEX i_v ON t(v)");
    db.execute("CREATE INDEX i_s ON t(s)");
    db.execute("CREATE INDEX i_w ON t(w)");
    db.execute("CREATE INDEX i_id ON t(id)");
    int used = 0;
    for (size_t i = 0; i < queries.size(); ++i) {
        auto r = db.execute(queries[i]);
        if (!r.stats.index_used.empty()) ++used;
        CHECK(sorted(r) == base[i]);
    }
    CHECK(used >= 8);  // the optimizer really used the indexes for selective queries

    // maintenance: insert / update / delete keep indexes consistent
    db.execute("INSERT INTO t VALUES (100000, 10, 1.5, 's7')");
    CHECK(db.execute("SELECT * FROM t WHERE id = 100000").data.rows.size() == 1);
    db.execute("UPDATE t SET v = 11 WHERE id = 100000");
    CHECK(db.execute("SELECT * FROM t WHERE id = 100000 AND v = 10").data.rows.empty());
    CHECK(db.execute("SELECT * FROM t WHERE id = 100000 AND v = 11").data.rows.size() == 1);
    db.execute("DELETE FROM t WHERE id = 100000");
    CHECK(db.execute("SELECT * FROM t WHERE id = 100000").data.rows.empty());
    // full re-check after churn against a brute-force scan (index dropped)
    auto with_idx = sorted(db.execute("SELECT * FROM t WHERE v >= 300 AND v <= 303"));
    db.execute("DROP INDEX i_v");
    CHECK(sorted(db.execute("SELECT * FROM t WHERE v >= 300 AND v <= 303")) == with_idx);
}

static void test_persistence_and_indexes() {
    std::string p = tmpPath("persist");
    {
        Database db(p, {16, 1 << 16});  // tiny pool forces eviction of dirty pages
        db.execute("CREATE TABLE c (id INT, name TEXT, age INT, city TEXT)");
        db.execute("GENERATE c 3000");
        db.execute("CREATE INDEX idx_c_id ON c(id)");
        db.execute("UPDATE c SET age = 77 WHERE id = 5");
        db.execute("DELETE FROM c WHERE id = 6");
    }
    Database db(p);
    auto r = db.execute("SELECT id, age FROM c WHERE id = 5");
    CHECK(r.data.rows.size() == 1 && std::get<int64_t>(r.data.rows[0][1]) == 77);
    CHECK(r.stats.index_used == "idx_c_id");  // index metadata survived and was rebuilt
    CHECK(db.execute("SELECT * FROM c WHERE id = 6").data.rows.empty());
    CHECK(db.execute("SELECT * FROM c").data.rows.size() == 2999);
    CHECK(db.execute("SHOW TABLES").data.rows.size() == 1);
}

static void test_cache() {
    Database db(tmpPath("cache"));
    db.execute("CREATE TABLE t (a INT, b TEXT)");
    db.execute("INSERT INTO t VALUES (1,'x'),(2,'y')");
    auto r1 = db.execute("SELECT * FROM t WHERE a = 1");
    CHECK(!r1.stats.cache_hit);
    auto r2 = db.execute("SELECT * FROM t WHERE a = 1");
    CHECK(r2.stats.cache_hit && r2.data.rows.size() == 1);
    CHECK(!db.execute("SELECT * FROM t WHERE a = 2").stats.cache_hit);  // different parameter -> different key
    db.execute("INSERT INTO t VALUES (1,'z')");                          // mutation invalidates
    auto r3 = db.execute("SELECT * FROM t WHERE a = 1");
    CHECK(!r3.stats.cache_hit && r3.data.rows.size() == 2);
    db.execute("UPDATE t SET b = 'q' WHERE a = 1");
    CHECK(db.execute("SELECT * FROM t WHERE a = 1").data.rows[0][1] == Value(std::string("q")));
    // byte budget: tiny cache evicts
    ResultCache c(600);
    QueryResult big;
    big.columns = {"c"};
    for (int i = 0; i < 5; ++i) big.rows.push_back({Value(std::string(20, 'a'))});
    for (int i = 0; i < 20; ++i) c.put("k" + std::to_string(i), "t", big);
    CHECK(c.bytesUsed() <= 600 && c.entries() < 20 && c.get("k19") != nullptr && c.get("k0") == nullptr);
}

static void test_adaptive_advisor() {
    Database db(tmpPath("adv"));
    db.execute("SET CACHE OFF");
    db.execute("CREATE TABLE customer (id INT, name TEXT, age INT, city TEXT)");
    db.execute("GENERATE customer 20000");
    for (int i = 1; i <= 40; ++i) db.execute("SELECT * FROM customer WHERE id = " + std::to_string(i * 37));  // selective
    for (int i = 0; i < 40; ++i) db.execute("SELECT * FROM customer WHERE age >= " + std::to_string(10 + i % 5) + " AND age < 60");  // ~50% of rows: an index cannot help
    CHECK(db.execute("EXPLAIN SELECT * FROM customer WHERE id = 5").message.find("Sequential Scan") != std::string::npos);
    auto recs = db.recommendations();
    bool id_rec = false, age_rec = false;
    for (auto& r : recs) {
        if (r.column == "id") id_rec = r.recommended;
        if (r.column == "age") age_rec = r.recommended;
    }
    CHECK(id_rec);
    CHECK(!age_rec);
    auto before = db.execute("SELECT * FROM customer WHERE id = 777");
    CHECK(before.stats.access_method == "sequential scan" && before.stats.rows_examined == 20000);
    db.execute("APPLY INDEX RECOMMENDATIONS");
    CHECK(db.hasIndexOn("customer", "id"));
    CHECK(!db.hasIndexOn("customer", "age"));
    auto after = db.execute("SELECT * FROM customer WHERE id = 777");
    CHECK(after.stats.access_method == "index scan" && after.stats.rows_examined == 1);
    CHECK(sorted(before) == sorted(after));
    CHECK(db.execute("EXPLAIN SELECT * FROM customer WHERE id = 5").message.find("Index Scan") != std::string::npos);
    // write-heavy workload makes an otherwise good index unattractive
    Database db2(tmpPath("adv2"));
    db2.execute("SET CACHE OFF");
    db2.execute("CREATE TABLE t (k INT, v INT)");
    db2.execute("GENERATE t 5000");
    for (int i = 1; i <= 6; ++i) db2.execute("SELECT * FROM t WHERE k = " + std::to_string(i));
    for (int i = 0; i < 400; ++i) db2.execute("INSERT INTO t VALUES (" + std::to_string(100000 + i) + ", 1)");
    for (auto& r : db2.recommendations()) if (r.column == "k") CHECK(!r.recommended);
}

static void test_tracker_bounded() {
    WorkloadTracker tr;
    std::vector<Column> cols = {{"a", Type::INT}};
    std::vector<RCond> conds = {{0, Op::EQ, Value(int64_t(1))}};
    QueryStats st;
    for (int i = 0; i < 5000; ++i) tr.recordRead("t", "fp" + std::to_string(i), cols, conds, "", st, 0.01);
    CHECK(tr.fingerprintCount() <= 1000);
    CHECK(tr.columns().at({"t", "a"}).total == 5000);
}

int main() {
    struct T { const char* n; void (*f)(); } tests[] = {
        {"disk persistence", test_disk_persistence}, {"buffer manager", test_buffer_manager}, {"heap file", test_heap},
        {"b+ tree", test_btree}, {"parser", test_parser}, {"errors leave db valid", test_errors_leave_db_valid},
        {"seq scan == index scan", test_seq_vs_index_equivalence}, {"restart persistence", test_persistence_and_indexes},
        {"result cache", test_cache}, {"adaptive advisor", test_adaptive_advisor}, {"tracker bounded", test_tracker_bounded}};
    for (auto& t : tests) {
        int before = g_fail;
        try { t.f(); } catch (const std::exception& e) { ++g_fail; std::cout << "  EXCEPTION: " << e.what() << "\n"; }
        std::cout << (g_fail == before ? "[ ok ] " : "[FAIL] ") << t.n << "\n";
    }
    std::cout << g_checks << " checks, " << g_fail << " failures\n";
    return g_fail ? 1 : 0;
}

