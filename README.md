# Adaptive Mini DBMS (C++17)

A small database engine with its own storage, B+ tree indexes, cost-based optimizer, and a
workload-aware index advisor that recommends indexes and then measures whether they helped.

## Build and run

Requires CMake 3.14+ and a C++17 compiler (GCC, Clang, or MSVC). No external database library or test framework is required.

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

With a single-configuration generator such as Ninja or Make:

```bash
./build/adaptive_minidb data/adaptive.db
```

On Windows with Visual Studio:

```powershell
.\build\Release\adaptive_minidb.exe data/adaptive.db
```

On Windows with Ninja, use `.\build\adaptive_minidb.exe` instead. The CLI creates the data directory and database when needed. End SQL statements with `;` and use `.quit` to exit.

### Benchmark

Run the benchmark from the build directory so its generated `adb_bench*.db` files remain inside the ignored build folder:

```bash
cd build
./workload_benchmark 100000
```

For Visual Studio, use `.\Release\workload_benchmark.exe 100000`. The benchmark compares sequential scans, a manual index, and an advisor-selected index, and reports measured latency, rows examined, page requests, and write overhead. Results depend on hardware and workload; no fixed speedup is promised. It recreates the three `adb_bench*.db` files in its working directory, so use a directory without valuable files of those names.

Without CMake:

```bash
g++ -std=c++17 -O2 -Iinclude src/database.cpp src/main.cpp -o adaptive_minidb
```

## Layout

| File | Module |
|---|---|
| `include/types.hpp` | Value (`std::variant`, monostate = NULL), RID, errors, binary serialization, stats |
| `include/storage.hpp` | `DiskManager` (4096-byte pages), `BufferManager` (LRU, pin counts, dirty flush), `PageGuard` |
| `include/heap.hpp` | Slotted-page heap file, RIDs, slot reuse and page compaction |
| `include/catalog.hpp` | Table/column/index metadata, persisted on page 0 |
| `include/bplus_tree.hpp` | B+ tree: duplicate keys via posting lists, linked leaves, range scans |
| `include/sql.hpp` | Tokenizer, AST, recursive-descent parser |
| `include/optimizer.hpp` | Selectivity estimation, cost model, plan choice (seq scan vs index scan) |
| `include/adaptive.hpp` | Workload tracker (bounded), index advisor, metrics |
| `include/cache.hpp` | LRU result cache with a byte budget and per-table invalidation |
| `src/database.cpp` | Executor + glue: everything behind `Database::execute(sql)` |
| `src/main.cpp` | CLI |
| `tests/all_tests.cpp`, `benchmarks/workload_benchmark.cpp` | Verification and measurement |

## SQL subset

```
CREATE TABLE t (col INT|DOUBLE|TEXT|BOOL, ...)      INSERT INTO t VALUES (...), (...)
SELECT * | c1, c2 FROM t [WHERE c op lit [AND ...]]  UPDATE t SET c = lit [, ...] [WHERE ...]
DELETE FROM t [WHERE ...]                            CREATE INDEX name ON t(col) / DROP INDEX name
EXPLAIN SELECT ...                                   SHOW TABLES / DESCRIBE t
SHOW INDEX RECOMMENDATIONS                           APPLY INDEX RECOMMENDATIONS   (explicit approval)
SHOW METRICS                                         SET CACHE ON|OFF
GENERATE t n     -- reproducible synthetic rows (seed 42) for demos and benchmarks
```
Operators: `= != <> < <= > >=`, combined with `AND` only. Names are case-insensitive (stored lowercase).

## Demo

```sql
CREATE TABLE Customer (id INT, name TEXT, age INT, city TEXT);
GENERATE Customer 100000;
SET CACHE OFF;
EXPLAIN SELECT * FROM Customer WHERE city = 'city_42';   -- Sequential Scan
-- ...run a workload of city queries...
SHOW INDEX RECOMMENDATIONS;                               -- explains benefit vs write/storage cost
CREATE INDEX idx_customer_city ON Customer(city);
EXPLAIN SELECT * FROM Customer WHERE city = 'city_42';   -- Index Scan (if estimated cheaper)
SHOW METRICS;                                             -- rows examined / ms by access path
```

## How the adaptive loop works

1. Every query updates aggregated per-column statistics (equality/range counts, observed selectivity,
   rows examined and latency split by access path). Query fingerprints are capped at 1000.
2. `S(c) = B_read - λw·W - λs·M` in page accesses: read savings vs. write maintenance vs. index storage.
   Frequency alone never triggers a recommendation; unselective columns are rejected
   (an unclustered index cannot beat a table scan when it must fetch about as many pages as the table has).
3. Indexes are created only by `CREATE INDEX` / `APPLY INDEX RECOMMENDATIONS`.
4. `SHOW METRICS` compares average rows examined and time before/after the index was used.

## Known limitations (deliberate, documented)

- **B+ tree nodes live in memory.** Index *definitions* persist in the catalog and the tree is rebuilt
  from the heap when the database opens (cost grows with table size). Nodes are not stored in pages yet.
- **Lazy B+ tree deletion**: entries are removed but nodes are not merged or redistributed. Lookups stay correct.
- **No crash recovery / WAL / transactions / concurrency.** A clean shutdown or DDL statement flushes pages;
  a crash mid-write can corrupt the file. Multi-row INSERT validates everything first (all-or-nothing on errors).
- The catalog must fit in one 4096-byte page. Records must fit in one page (~4 KB).
- `AND` only (no `OR`, joins, subqueries, ORDER BY, aggregates). NULLs never match comparisons.
- Serialization assumes a little-endian host.
- Statistics (distinct counts, min/max) are refreshed lazily, so estimates can be stale after heavy writes.
- The result cache invalidates per table on any write (coarse but always correct).
- Bulk `GENERATE` loads are not counted as workload writes, so they do not penalize index recommendations.

## Repository hygiene

Generated databases, compiled binaries, and CMake caches from the original archive are excluded. The repository contains source code, tests, and a reproducible synthetic-data generator. No license was supplied; choose one after confirming ownership and intended reuse rights.

## Validation

Built from source on Windows with GCC 15.2 and Ninja. The included CTest suite passed, and the benchmark completed with 5,000 synthetic rows, exercising manual and advisor-created indexes. GCC emitted an array-bounds warning in the optimized heap test; the test suite passed, but the warning remains for investigation.

