// Common types shared by every module: values, rows, record IDs, errors, stats.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace adb {

constexpr uint32_t PAGE_SIZE = 4096;
using PageId = uint32_t;
constexpr PageId INVALID_PAGE = 0xFFFFFFFFu;

// ---- error categories -------------------------------------------------------
struct DbError : std::runtime_error { using std::runtime_error::runtime_error; };
struct ParseError : DbError { using DbError::DbError; };
struct SchemaError : DbError { using DbError::DbError; };
struct StorageError : DbError { using DbError::DbError; };
struct IndexError : DbError { using DbError::DbError; };
struct ExecutionError : DbError { using DbError::DbError; };
struct CacheError : DbError { using DbError::DbError; };

// ---- column types and values ------------------------------------------------
enum class Type { INT, DOUBLE, TEXT, BOOL };
inline const char* typeName(Type t) {
    switch (t) {
        case Type::INT: return "INT";
        case Type::DOUBLE: return "DOUBLE";
        case Type::TEXT: return "TEXT";
        case Type::BOOL: return "BOOL";
    }
    return "?";
}

struct Column {
    std::string name;
    Type type;
};

// monostate == SQL NULL.  NOTE: always build text values with std::string(...);
// a bare "literal" would silently convert to bool inside a std::variant.
using Value = std::variant<std::monostate, int64_t, double, std::string, bool>;
using Row = std::vector<Value>;

inline bool isNull(const Value& v) { return std::holds_alternative<std::monostate>(v); }
inline bool isNumeric(const Value& v) {
    return std::holds_alternative<int64_t>(v) || std::holds_alternative<double>(v);
}
inline double asDouble(const Value& v) {
    if (auto* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
    return std::get<double>(v);
}

// Three-way comparison with explicit type rules (never compares via strings).
inline int compare(const Value& a, const Value& b) {
    if (isNumeric(a) && isNumeric(b)) {
        if (std::holds_alternative<int64_t>(a) && std::holds_alternative<int64_t>(b)) {
            int64_t x = std::get<int64_t>(a), y = std::get<int64_t>(b);
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        double x = asDouble(a), y = asDouble(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.index() != b.index() || isNull(a)) throw ExecutionError("incomparable values");
    if (auto* s = std::get_if<std::string>(&a)) {
        int c = s->compare(std::get<std::string>(b));
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    return static_cast<int>(std::get<bool>(a)) - static_cast<int>(std::get<bool>(b));
}

inline std::string formatValue(const Value& v) {
    if (isNull(v)) return "NULL";
    if (auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
    if (auto* d = std::get_if<double>(&v)) {
        std::ostringstream o;
        o << std::setprecision(10) << *d;
        return o.str();
    }
    if (auto* s = std::get_if<std::string>(&v)) return *s;
    return std::get<bool>(v) ? "true" : "false";
}

// SQL-literal form, used for cache keys / EXPLAIN.
inline std::string literal(const Value& v) {
    if (auto* s = std::get_if<std::string>(&v)) {
        std::string o = "'";
        for (char c : *s) { if (c == '\'') o += '\''; o += c; }
        return o + "'";
    }
    if (auto* d = std::get_if<double>(&v)) {
        std::ostringstream o;
        o << std::setprecision(17) << *d << "d";
        return o.str();
    }
    if (isNull(v)) return "NULL";
    return formatValue(v);
}

// ---- binary serialization (little-endian host assumed; documented limitation) -
// value := tag(1) payload;  tags: 0 NULL, 1 INT(8), 2 DOUBLE(8), 3 TEXT(len u32 + bytes), 4 BOOL(1)
inline void putValue(std::string& out, const Value& v) {
    out.push_back(static_cast<char>(v.index()));
    switch (v.index()) {
        case 1: { int64_t x = std::get<int64_t>(v); out.append(reinterpret_cast<char*>(&x), 8); break; }
        case 2: { double x = std::get<double>(v); out.append(reinterpret_cast<char*>(&x), 8); break; }
        case 3: {
            const auto& s = std::get<std::string>(v);
            uint32_t n = static_cast<uint32_t>(s.size());
            out.append(reinterpret_cast<char*>(&n), 4);
            out.append(s);
            break;
        }
        case 4: out.push_back(std::get<bool>(v) ? 1 : 0); break;
        default: break;
    }
}

inline Value getValue(const char*& p, const char* end) {
    auto need = [&](size_t n) { if (static_cast<size_t>(end - p) < n) throw StorageError("corrupt record"); };
    need(1);
    uint8_t tag = static_cast<uint8_t>(*p++);
    switch (tag) {
        case 0: return Value{};
        case 1: { need(8); int64_t x; std::memcpy(&x, p, 8); p += 8; return Value(x); }
        case 2: { need(8); double x; std::memcpy(&x, p, 8); p += 8; return Value(x); }
        case 3: {
            need(4); uint32_t n; std::memcpy(&n, p, 4); p += 4;
            need(n); std::string s(p, n); p += n;
            return Value(std::move(s));
        }
        case 4: { need(1); bool b = (*p++ != 0); return Value(b); }
    }
    throw StorageError("corrupt record: bad tag");
}

// ---- record identifiers -------------------------------------------------------
struct RID {
    PageId page = INVALID_PAGE;
    uint16_t slot = 0;
    bool operator==(const RID& o) const { return page == o.page && slot == o.slot; }
};

// ---- results and diagnostics (kept separate so the cache never stores stats) ---
struct QueryResult {
    std::vector<std::string> columns;
    std::vector<Row> rows;
};

struct QueryStats {
    double exec_ms = 0;
    uint64_t rows_examined = 0, rows_returned = 0;
    uint64_t logical_reads = 0, physical_reads = 0, buffer_hits = 0, buffer_misses = 0;
    std::string access_method = "n/a";
    std::string index_used;
    bool cache_hit = false;
};

}  // namespace adb

