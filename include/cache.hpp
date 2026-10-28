// LRU result cache with a byte budget.  Entries depend on one table and are
// invalidated whenever that table is modified.
#pragma once
#include <list>
#include <unordered_map>

#include "types.hpp"

namespace adb {

class ResultCache {
public:
    explicit ResultCache(size_t max_bytes) : max_bytes_(max_bytes) {}

    const QueryResult* get(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) { ++misses_; return nullptr; }
        lru_.splice(lru_.begin(), lru_, it->second);  // move to front (most recent)
        ++hits_;
        return &it->second->result;
    }
    void put(const std::string& key, const std::string& table, const QueryResult& r) {
        size_t bytes = key.size() + sizeOf(r);
        if (bytes > max_bytes_) return;  // never cache results bigger than the whole budget
        erase(key);
        lru_.push_front(Entry{key, table, r, bytes});
        map_[key] = lru_.begin();
        used_ += bytes;
        while (used_ > max_bytes_) {  // evict least recently used
            const Entry& e = lru_.back();
            used_ -= e.bytes;
            map_.erase(e.key);
            lru_.pop_back();
        }
    }
    void invalidateTable(const std::string& table) {
        for (auto it = lru_.begin(); it != lru_.end();) {
            if (it->table == table) { used_ -= it->bytes; map_.erase(it->key); it = lru_.erase(it); }
            else ++it;
        }
    }
    void clear() { lru_.clear(); map_.clear(); used_ = 0; }
    size_t entries() const { return lru_.size(); }
    size_t bytesUsed() const { return used_; }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

private:
    struct Entry { std::string key, table; QueryResult result; size_t bytes; };
    static size_t sizeOf(const QueryResult& r) {
        size_t b = 64;
        for (auto& c : r.columns) b += c.size() + 32;
        for (auto& row : r.rows) {
            b += 24;
            for (auto& v : row) b += sizeof(Value) + (std::holds_alternative<std::string>(v) ? std::get<std::string>(v).size() : 0);
        }
        return b;
    }
    void erase(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return;
        used_ -= it->second->bytes;
        lru_.erase(it->second);
        map_.erase(it);
    }
    size_t max_bytes_, used_ = 0;
    uint64_t hits_ = 0, misses_ = 0;
    std::list<Entry> lru_;
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
};

}  // namespace adb

