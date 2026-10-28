// Disk manager (fixed 4096-byte pages) and buffer manager (LRU, pin counts, dirty pages).
#pragma once
#include <fstream>
#include <list>
#include <unordered_map>

#include "types.hpp"

namespace adb {

class DiskManager {
public:
    explicit DiskManager(const std::string& path) {
        file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open()) {  // create it
            file_.clear();
            std::ofstream create(path, std::ios::binary);
            create.close();
            file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
        }
        if (!file_.is_open()) throw StorageError("cannot open database file: " + path);
        file_.seekg(0, std::ios::end);
        auto size = static_cast<uint64_t>(file_.tellg());
        if (size % PAGE_SIZE) throw StorageError("database file size is not a multiple of the page size");
        num_pages_ = static_cast<PageId>(size / PAGE_SIZE);
    }
    ~DiskManager() { if (file_.is_open()) file_.flush(); }

    PageId numPages() const { return num_pages_; }

    PageId allocatePage() {
        static const std::vector<char> zero(PAGE_SIZE, 0);
        PageId id = num_pages_;
        write(id, zero.data());
        ++num_pages_;
        return id;
    }
    void readPage(PageId id, char* dst) {
        if (id >= num_pages_) throw StorageError("read of unallocated page " + std::to_string(id));
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(id) * PAGE_SIZE);
        file_.read(dst, PAGE_SIZE);
        if (file_.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) throw StorageError("short read on page " + std::to_string(id));
        ++reads_;
    }
    void writePage(PageId id, const char* src) {
        if (id >= num_pages_) throw StorageError("write of unallocated page " + std::to_string(id));
        write(id, src);
    }
    void flush() { file_.flush(); }
    uint64_t reads() const { return reads_; }
    uint64_t writes() const { return writes_; }

private:
    void write(PageId id, const char* src) {
        file_.clear();
        file_.seekp(static_cast<std::streamoff>(id) * PAGE_SIZE);
        file_.write(src, PAGE_SIZE);
        if (!file_) throw StorageError("write failed on page " + std::to_string(id));
        ++writes_;
    }
    std::fstream file_;
    PageId num_pages_ = 0;
    uint64_t reads_ = 0, writes_ = 0;
};

class BufferManager {
public:
    BufferManager(DiskManager& disk, size_t capacity) : disk_(disk), frames_(capacity) {
        if (capacity < 2) throw StorageError("buffer pool needs at least 2 frames");
        for (size_t i = 0; i < capacity; ++i) { frames_[i].data.resize(PAGE_SIZE); free_.push_back(i); }
    }
    ~BufferManager() { try { flushAll(); } catch (...) {} }

    // Returns a pinned page; caller must unpin().
    char* fetch(PageId id) {
        auto it = table_.find(id);
        if (it != table_.end()) {
            ++hits_;
            Frame& f = frames_[it->second];
            if (f.pin == 0) lru_.erase(f.lru_it);
            ++f.pin;
            return f.data.data();
        }
        ++misses_;
        size_t fi = victim();
        Frame& f = frames_[fi];
        try { disk_.readPage(id, f.data.data()); } catch (...) { free_.push_back(fi); throw; }
        f.id = id; f.dirty = false; f.pin = 1;
        table_[id] = fi;
        return f.data.data();
    }
    char* newPage(PageId& id) {
        size_t fi = victim();
        try { id = disk_.allocatePage(); } catch (...) { free_.push_back(fi); throw; }
        Frame& f = frames_[fi];
        std::fill(f.data.begin(), f.data.end(), 0);
        f.id = id; f.dirty = true; f.pin = 1;
        table_[id] = fi;
        return f.data.data();
    }
    void unpin(PageId id, bool dirty) {
        auto it = table_.find(id);
        if (it == table_.end()) throw StorageError("unpin of page not in pool");
        Frame& f = frames_[it->second];
        if (f.pin <= 0) throw StorageError("unpin of unpinned page");
        f.dirty |= dirty;
        if (--f.pin == 0) { lru_.push_back(it->second); f.lru_it = std::prev(lru_.end()); }
    }
    void flushPage(PageId id) {
        auto it = table_.find(id);
        if (it == table_.end()) return;
        Frame& f = frames_[it->second];
        if (f.dirty) { disk_.writePage(f.id, f.data.data()); f.dirty = false; }
    }
    void flushAll() {
        for (auto& kv : table_) flushPage(kv.first);
        disk_.flush();
    }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    DiskManager& disk() { return disk_; }

private:
    struct Frame {
        PageId id = INVALID_PAGE;
        std::vector<char> data;
        bool dirty = false;
        int pin = 0;
        std::list<size_t>::iterator lru_it;
    };
    size_t victim() {
        if (!free_.empty()) { size_t fi = free_.back(); free_.pop_back(); return fi; }
        if (lru_.empty()) throw StorageError("buffer pool exhausted: every frame is pinned");
        size_t fi = lru_.front();  // front = least recently used
        Frame& f = frames_[fi];
        if (f.dirty) { disk_.writePage(f.id, f.data.data()); f.dirty = false; }
        lru_.pop_front();
        table_.erase(f.id);
        return fi;
    }
    DiskManager& disk_;
    std::vector<Frame> frames_;
    std::unordered_map<PageId, size_t> table_;
    std::list<size_t> lru_;  // unpinned frames only, oldest first
    std::vector<size_t> free_;
    uint64_t hits_ = 0, misses_ = 0;
};

// RAII pin: guarantees unpin even when exceptions propagate.
class PageGuard {
public:
    PageGuard(BufferManager& bm, PageId id) : bm_(&bm), id_(id), data_(bm.fetch(id)) {}
    static PageGuard create(BufferManager& bm) {
        PageId id;
        char* d = bm.newPage(id);
        return PageGuard(bm, id, d);
    }
    PageGuard(PageGuard&& o) noexcept : bm_(o.bm_), id_(o.id_), data_(o.data_), dirty_(o.dirty_) { o.bm_ = nullptr; }
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    ~PageGuard() { if (bm_) bm_->unpin(id_, dirty_); }
    char* data() { return data_; }
    const char* data() const { return data_; }
    PageId id() const { return id_; }
    void markDirty() { dirty_ = true; }

private:
    PageGuard(BufferManager& bm, PageId id, char* d) : bm_(&bm), id_(id), data_(d), dirty_(true) {}
    BufferManager* bm_;
    PageId id_;
    char* data_;
    bool dirty_ = false;
};

}  // namespace adb

