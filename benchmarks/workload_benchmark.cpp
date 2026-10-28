// Compares: no index vs manual index vs advisor-chosen index on the same point-query workload.
// All numbers are measured at run time; nothing is hard-coded.  Usage: workload_benchmark [rows]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>

#include "database.hpp"

using namespace adb;
using Clock = std::chrono::steady_clock;

struct Sample { double median_ms, p95_ms, examined, page_requests; };

static Sample runWorkload(Database& db, int64_t rows, int n) {
    std::vector<double> ms;
    double examined = 0, pages = 0;
    for (int i = 0; i < n; ++i) {
        int64_t id = 1 + (i * 7919) % rows;  // deterministic spread of keys
        auto r = db.execute("SELECT * FROM customer WHERE id = " + std::to_string(id));
        ms.push_back(r.stats.exec_ms);
        examined += static_cast<double>(r.stats.rows_examined);
        pages += static_cast<double>(r.stats.logical_reads);
    }
    std::sort(ms.begin(), ms.end());
    return {ms[ms.size() / 2], ms[static_cast<size_t>(ms.size() * 0.95)], examined / n, pages / n};
}

static void report(const char* name, const Sample& s) {
    std::printf("%-28s median %8.4f ms   p95 %8.4f ms   rows examined/query %9.1f   page requests/query %8.1f\n", name, s.median_ms, s.p95_ms,
                s.examined, s.page_requests);
}

int main(int argc, char** argv) {
    int64_t rows = argc > 1 ? std::atoll(argv[1]) : 100000;
    if (rows <= 0) {
        std::cerr << "Row count must be a positive integer.\n";
        return 1;
    }
    const int N = 200;
    std::string path = "adb_bench.db";
    std::remove(path.c_str());
    Database db(path, {512, 1 << 20});
    db.execute("SET CACHE OFF");  // measure the engine, not the result cache
    db.execute("CREATE TABLE customer (id INT, name TEXT, age INT, city TEXT)");
    db.execute("GENERATE customer " + std::to_string(rows));
    std::printf("rows=%lld pages=%lld\n", static_cast<long long>(rows),
                static_cast<long long>(std::get<int64_t>(db.execute("SHOW TABLES").data.rows[0][3])));

    runWorkload(db, rows, 5);  // warm-up
    Sample seq = runWorkload(db, rows, N);
    report("A. no index (seq scan)", seq);

    auto t0 = Clock::now();
    db.execute("CREATE INDEX idx_id ON customer(id)");
    double build_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    Sample idx = runWorkload(db, rows, N);
    report("B. manual index on id", idx);
    std::printf("   index build time %.1f ms; speedup (median) %.1fx; latency improvement %.1f%%\n", build_ms,
                seq.median_ms / std::max(idx.median_ms, 1e-9), (seq.median_ms - idx.median_ms) / seq.median_ms * 100.0);

    // C. adaptive: fresh database, let the workload teach the advisor
    std::remove("adb_bench2.db");
    Database db2("adb_bench2.db", {512, 1 << 20});
    db2.execute("SET CACHE OFF");
    db2.execute("CREATE TABLE customer (id INT, name TEXT, age INT, city TEXT)");
    db2.execute("GENERATE customer " + std::to_string(rows));
    Sample phase1 = runWorkload(db2, rows, N);
    report("C1. adaptive, before advice", phase1);
    std::cout << db2.execute("APPLY INDEX RECOMMENDATIONS").message;
    Sample phase2 = runWorkload(db2, rows, N);
    report("C2. adaptive, after advice", phase2);

    // write overhead of maintaining the index
    auto timeInserts = [&](Database& d, int base) {
        auto s = Clock::now();
        for (int i = 0; i < 2000; ++i)
            d.execute("INSERT INTO customer VALUES (" + std::to_string(base + i) + ", 'n', 30, 'c')");
        return std::chrono::duration<double, std::milli>(Clock::now() - s).count();
    };
    std::remove("adb_bench3.db");
    Database plain("adb_bench3.db", {512, 1 << 20});
    plain.execute("CREATE TABLE customer (id INT, name TEXT, age INT, city TEXT)");
    double t_plain = timeInserts(plain, 1);
    plain.execute("CREATE INDEX i ON customer(id)");
    double t_idx = timeInserts(plain, 100000);
    std::printf("write overhead: 2000 inserts %.1f ms without index vs %.1f ms with index\n", t_plain, t_idx);
    return 0;
}

