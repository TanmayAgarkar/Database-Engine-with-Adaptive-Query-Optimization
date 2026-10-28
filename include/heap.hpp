// Heap file of slotted pages.  One TableHeap == one table; pages form a linked chain.
//
// Page layout (4096 bytes):
//   [0..3]  next page id     [4..5] slot count     [6..7] free-space end offset
//   [8..]   slot directory, 4 bytes/slot: {offset u16, length u16}   (length 0 == dead slot)
//   record bytes grow downward from the end of the page.
// Slot numbers never change, so RIDs stay valid even when a page is compacted.
#pragma once
#include "storage.hpp"

namespace adb {

class TableHeap {
public:
    static constexpr size_t HDR = 8, SLOT = 4;
    static constexpr size_t MAX_RECORD = PAGE_SIZE - HDR - SLOT;

    static PageId create(BufferManager& bm) {
        auto g = PageGuard::create(bm);
        initPage(g.data());
        return g.id();
    }
    TableHeap(BufferManager& bm, PageId first) : bm_(bm), first_(first), last_(first) {
        // Rebuild in-memory bookkeeping by walking the chain.
        PageId p = first;
        pages_ = 0; rows_ = 0;
        while (p != INVALID_PAGE) {
            PageGuard g(bm_, p);
            const char* d = g.data();
            uint16_t ns = numSlots(d);
            for (uint16_t i = 0; i < ns; ++i) if (slotLen(d, i)) ++rows_;
            ++pages_;
            last_ = p;
            p = nextPage(d);
        }
    }

    PageId firstPage() const { return first_; }
    size_t numPages() const { return pages_; }
    size_t numRows() const { return rows_; }

    RID insert(const std::string& rec) {
        if (rec.size() > MAX_RECORD) throw StorageError("record too large for one page");
        RID r;
        if (tryInsert(last_, rec, r)) { ++rows_; return r; }
        for (size_t i = 0; i < hints_.size();) {
            if (tryInsert(hints_[i], rec, r)) { ++rows_; return r; }
            hints_.erase(hints_.begin() + static_cast<long>(i));
        }
        PageId np;
        { auto g = PageGuard::create(bm_); np = g.id(); initPage(g.data()); }
        { PageGuard g(bm_, last_); setNext(g.data(), np); g.markDirty(); }
        last_ = np; ++pages_;
        if (!tryInsert(last_, rec, r)) throw StorageError("insert into fresh page failed");
        ++rows_;
        return r;
    }

    bool get(RID rid, std::string& out) const {
        PageGuard g(bm_, rid.page);
        const char* d = g.data();
        if (rid.slot >= numSlots(d) || !slotLen(d, rid.slot)) return false;
        out.assign(d + slotOff(d, rid.slot), slotLen(d, rid.slot));
        return true;
    }

    bool erase(RID rid) {
        PageGuard g(bm_, rid.page);
        char* d = g.data();
        if (rid.slot >= numSlots(d) || !slotLen(d, rid.slot)) return false;
        setSlot(d, rid.slot, 0, 0);
        g.markDirty();
        --rows_;
        if (hints_.empty() || hints_.back() != rid.page) hints_.push_back(rid.page);
        return true;
    }

    // Calls f(RID, const char* data, size_t len) for every live record.  The page stays
    // pinned during the call, so f must not modify the table.
    template <class F>
    void scan(F&& f) const {
        PageId p = first_;
        while (p != INVALID_PAGE) {
            PageGuard g(bm_, p);
            const char* d = g.data();
            uint16_t ns = numSlots(d);
            for (uint16_t i = 0; i < ns; ++i)
                if (uint16_t len = slotLen(d, i)) f(RID{p, i}, d + slotOff(d, i), static_cast<size_t>(len));
            p = nextPage(d);
        }
    }

private:
    // --- raw page accessors ---
    static uint32_t nextPage(const char* d) { uint32_t v; std::memcpy(&v, d, 4); return v; }
    static void setNext(char* d, uint32_t v) { std::memcpy(d, &v, 4); }
    static uint16_t numSlots(const char* d) { uint16_t v; std::memcpy(&v, d + 4, 2); return v; }
    static void setNumSlots(char* d, uint16_t v) { std::memcpy(d + 4, &v, 2); }
    static uint16_t freeEnd(const char* d) { uint16_t v; std::memcpy(&v, d + 6, 2); return v; }
    static void setFreeEnd(char* d, uint16_t v) { std::memcpy(d + 6, &v, 2); }
    static uint16_t slotOff(const char* d, size_t i) { uint16_t v; std::memcpy(&v, d + HDR + i * SLOT, 2); return v; }
    static uint16_t slotLen(const char* d, size_t i) { uint16_t v; std::memcpy(&v, d + HDR + i * SLOT + 2, 2); return v; }
    static void setSlot(char* d, size_t i, uint16_t off, uint16_t len) {
        std::memcpy(d + HDR + i * SLOT, &off, 2);
        std::memcpy(d + HDR + i * SLOT + 2, &len, 2);
    }
    static void initPage(char* d) {
        std::memset(d, 0, PAGE_SIZE);
        setNext(d, INVALID_PAGE);
        setNumSlots(d, 0);
        setFreeEnd(d, static_cast<uint16_t>(PAGE_SIZE));
    }
    // Repack live records against the end of the page (slot numbers are unchanged).
    static void compact(char* d) {
        uint16_t ns = numSlots(d);
        std::vector<char> tmp(PAGE_SIZE);
        size_t end = PAGE_SIZE;
        std::vector<std::pair<uint16_t, uint16_t>> newSlots(ns, {0, 0});
        for (uint16_t i = 0; i < ns; ++i) {
            uint16_t len = slotLen(d, i);
            if (!len) continue;
            end -= len;
            std::memcpy(tmp.data() + end, d + slotOff(d, i), len);
            newSlots[i] = {static_cast<uint16_t>(end), len};
        }
        std::memcpy(d + end, tmp.data() + end, PAGE_SIZE - end);
        for (uint16_t i = 0; i < ns; ++i) setSlot(d, i, newSlots[i].first, newSlots[i].second);
        setFreeEnd(d, static_cast<uint16_t>(end));
    }

    bool tryInsert(PageId pid, const std::string& rec, RID& out) {
        PageGuard g(bm_, pid);
        char* d = g.data();
        uint16_t ns = numSlots(d);
        int dead = -1;
        size_t live = 0;
        for (uint16_t i = 0; i < ns; ++i) {
            if (slotLen(d, i)) live += slotLen(d, i); else if (dead < 0) dead = i;
        }
        size_t need = rec.size() + (dead < 0 ? SLOT : 0);
        size_t dir_end = HDR + static_cast<size_t>(ns) * SLOT;
        if (freeEnd(d) - dir_end < need) {
            // contiguous space too small: reclaim dead-record bytes if that would be enough
            size_t total_free = PAGE_SIZE - dir_end - live;
            if (total_free < need) return false;
            compact(d);
            g.markDirty();
        }
        uint16_t off = static_cast<uint16_t>(freeEnd(d) - rec.size());
        std::memcpy(d + off, rec.data(), rec.size());
        setFreeEnd(d, off);
        uint16_t slot = dead >= 0 ? static_cast<uint16_t>(dead) : ns;
        if (dead < 0) setNumSlots(d, static_cast<uint16_t>(ns + 1));
        setSlot(d, slot, off, static_cast<uint16_t>(rec.size()));
        g.markDirty();
        out = RID{pid, slot};
        return true;
    }

    BufferManager& bm_;
    PageId first_, last_;
    size_t pages_ = 0, rows_ = 0;
    std::vector<PageId> hints_;  // pages that had deletions (candidates for reuse)
};

}  // namespace adb

