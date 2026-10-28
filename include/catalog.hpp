// Catalog: table schemas and index definitions.  Persisted as text on page 0.
#pragma once
#include <map>

#include "types.hpp"

namespace adb {

struct TableMeta {
    std::string name;
    std::vector<Column> cols;
    PageId first_page = INVALID_PAGE;
    int colIndex(const std::string& c) const {
        for (size_t i = 0; i < cols.size(); ++i) if (cols[i].name == c) return static_cast<int>(i);
        return -1;
    }
};

struct IndexMeta {
    std::string name, table, column;
};

class Catalog {
public:
    bool hasTable(const std::string& n) const { return tables_.count(n) > 0; }
    const TableMeta& table(const std::string& n) const {
        auto it = tables_.find(n);
        if (it == tables_.end()) throw SchemaError("unknown table: " + n);
        return it->second;
    }
    void addTable(const TableMeta& t) {
        if (hasTable(t.name)) throw SchemaError("table already exists: " + t.name);
        tables_[t.name] = t;
    }
    const std::map<std::string, TableMeta>& tables() const { return tables_; }

    bool hasIndex(const std::string& n) const { return indexes_.count(n) > 0; }
    const IndexMeta& index(const std::string& n) const {
        auto it = indexes_.find(n);
        if (it == indexes_.end()) throw SchemaError("unknown index: " + n);
        return it->second;
    }
    void addIndex(const IndexMeta& m) {
        if (hasIndex(m.name)) throw SchemaError("index already exists: " + m.name);
        indexes_[m.name] = m;
    }
    void dropIndex(const std::string& n) { if (!indexes_.erase(n)) throw SchemaError("unknown index: " + n); }
    const std::map<std::string, IndexMeta>& indexes() const { return indexes_; }

    // Line format:  T name first_page ncols / C colname type / I idxname table column
    std::string serialize() const {
        std::ostringstream o;
        for (auto& [n, t] : tables_) {
            o << "T " << n << ' ' << t.first_page << ' ' << t.cols.size() << '\n';
            for (auto& c : t.cols) o << "C " << c.name << ' ' << static_cast<int>(c.type) << '\n';
        }
        for (auto& [n, i] : indexes_) o << "I " << n << ' ' << i.table << ' ' << i.column << '\n';
        return o.str();
    }
    void deserialize(const std::string& s) {
        tables_.clear(); indexes_.clear();
        std::istringstream in(s);
        std::string tag;
        while (in >> tag) {
            if (tag == "T") {
                TableMeta t; size_t n;
                in >> t.name >> t.first_page >> n;
                for (size_t i = 0; i < n; ++i) {
                    std::string c, ctag; int ty;
                    in >> ctag >> c >> ty;
                    if (ctag != "C") throw StorageError("corrupt catalog");
                    t.cols.push_back({c, static_cast<Type>(ty)});
                }
                tables_[t.name] = t;
            } else if (tag == "I") {
                IndexMeta m;
                in >> m.name >> m.table >> m.column;
                indexes_[m.name] = m;
            } else {
                throw StorageError("corrupt catalog");
            }
        }
    }

private:
    std::map<std::string, TableMeta> tables_;
    std::map<std::string, IndexMeta> indexes_;
};

}  // namespace adb

