// B+ tree mapping a Value key -> list of RIDs (duplicate keys share one posting list).
// Leaves are linked for range scans.  Deletion removes entries lazily (no merge /
// redistribution): separators stay valid, so lookups remain correct.
#pragma once
#include <algorithm>
#include <memory>

#include "types.hpp"

namespace adb {

class BPlusTree {
    struct Node {
        bool leaf;
        std::vector<Value> keys;
        std::vector<std::unique_ptr<Node>> children;  // internal: keys.size()+1 children
        std::vector<std::vector<RID>> rids;           // leaf: one posting list per key
        Node* next = nullptr;
        explicit Node(bool l) : leaf(l) {}
    };
    struct Split { Value key; std::unique_ptr<Node> right; };

public:
    explicit BPlusTree(size_t max_keys = 64) : root_(new Node(true)), max_keys_(max_keys) {
        if (max_keys < 3) throw IndexError("B+ tree needs at least 3 keys per node");
    }

    void insert(const Value& key, RID rid) {
        if (isNull(key)) throw IndexError("NULL keys are not indexed");
        auto sp = insertRec(root_.get(), key, rid);
        if (sp) {
            auto nr = std::make_unique<Node>(false);
            nr->keys.push_back(sp->key);
            nr->children.push_back(std::move(root_));
            nr->children.push_back(std::move(sp->right));
            root_ = std::move(nr);
            ++height_;
        }
    }

    std::vector<RID> search(const Value& key) const { return range(key, true, key, true); }

    std::vector<RID> range(const std::optional<Value>& lo, bool lo_inc,
                           const std::optional<Value>& hi, bool hi_inc) const {
        std::vector<RID> out;
        const Node* n = lo ? findLeaf(*lo) : leftmost();
        size_t i = lo ? lowerBound(n->keys, *lo) : 0;
        for (; n; n = n->next, i = 0) {
            for (; i < n->keys.size(); ++i) {
                const Value& k = n->keys[i];
                if (lo && !lo_inc && compare(k, *lo) == 0) continue;
                if (hi) {
                    int c = compare(k, *hi);
                    if (c > 0 || (c == 0 && !hi_inc)) return out;
                }
                out.insert(out.end(), n->rids[i].begin(), n->rids[i].end());
            }
        }
        return out;
    }

    bool erase(const Value& key, RID rid) {
        if (isNull(key)) return false;
        Node* n = const_cast<Node*>(findLeaf(key));
        size_t i = lowerBound(n->keys, key);
        if (i >= n->keys.size() || compare(n->keys[i], key) != 0) return false;
        auto& list = n->rids[i];
        auto it = std::find(list.begin(), list.end(), rid);
        if (it == list.end()) return false;
        list.erase(it);
        --entries_;
        if (list.empty()) {
            n->keys.erase(n->keys.begin() + static_cast<long>(i));
            n->rids.erase(n->rids.begin() + static_cast<long>(i));
        }
        return true;
    }

    size_t size() const { return entries_; }
    size_t height() const { return height_; }
    size_t leafCount() const {
        size_t c = 0;
        for (const Node* n = leftmost(); n; n = n->next) ++c;
        return c;
    }
    // Test helper: keys strictly increase along the leaf chain and entry count matches.
    bool checkInvariants() const {
        size_t total = 0;
        const Value* prev = nullptr;
        for (const Node* n = leftmost(); n; n = n->next) {
            for (size_t i = 0; i < n->keys.size(); ++i) {
                if (prev && compare(*prev, n->keys[i]) >= 0) return false;
                prev = &n->keys[i];
                if (n->rids[i].empty()) return false;
                total += n->rids[i].size();
            }
        }
        return total == entries_;
    }

private:
    static size_t lowerBound(const std::vector<Value>& keys, const Value& k) {
        return static_cast<size_t>(std::lower_bound(keys.begin(), keys.end(), k,
            [](const Value& e, const Value& x) { return compare(e, x) < 0; }) - keys.begin());
    }
    static size_t childIndex(const Node* n, const Value& k) {
        return static_cast<size_t>(std::upper_bound(n->keys.begin(), n->keys.end(), k,
            [](const Value& x, const Value& e) { return compare(x, e) < 0; }) - n->keys.begin());
    }
    const Node* findLeaf(const Value& k) const {
        const Node* n = root_.get();
        while (!n->leaf) n = n->children[childIndex(n, k)].get();
        return n;
    }
    const Node* leftmost() const {
        const Node* n = root_.get();
        while (!n->leaf) n = n->children[0].get();
        return n;
    }

    std::optional<Split> insertRec(Node* n, const Value& key, RID rid) {
        if (n->leaf) {
            size_t i = lowerBound(n->keys, key);
            if (i < n->keys.size() && compare(n->keys[i], key) == 0) {
                n->rids[i].push_back(rid);
            } else {
                n->keys.insert(n->keys.begin() + static_cast<long>(i), key);
                n->rids.insert(n->rids.begin() + static_cast<long>(i), std::vector<RID>{rid});
            }
            ++entries_;
            if (n->keys.size() <= max_keys_) return std::nullopt;
            size_t mid = n->keys.size() / 2;
            auto right = std::make_unique<Node>(true);
            right->keys.assign(std::make_move_iterator(n->keys.begin() + static_cast<long>(mid)), std::make_move_iterator(n->keys.end()));
            right->rids.assign(std::make_move_iterator(n->rids.begin() + static_cast<long>(mid)), std::make_move_iterator(n->rids.end()));
            n->keys.resize(mid);
            n->rids.resize(mid);
            right->next = n->next;
            n->next = right.get();
            Value sep = right->keys.front();
            return Split{sep, std::move(right)};
        }
        size_t i = childIndex(n, key);
        auto sp = insertRec(n->children[i].get(), key, rid);
        if (!sp) return std::nullopt;
        n->keys.insert(n->keys.begin() + static_cast<long>(i), sp->key);
        n->children.insert(n->children.begin() + static_cast<long>(i) + 1, std::move(sp->right));
        if (n->keys.size() <= max_keys_) return std::nullopt;
        size_t mid = n->keys.size() / 2;
        Value sep = n->keys[mid];
        auto right = std::make_unique<Node>(false);
        right->keys.assign(std::make_move_iterator(n->keys.begin() + static_cast<long>(mid) + 1), std::make_move_iterator(n->keys.end()));
        right->children.assign(std::make_move_iterator(n->children.begin() + static_cast<long>(mid) + 1), std::make_move_iterator(n->children.end()));
        n->keys.resize(mid);
        n->children.resize(mid + 1);
        return Split{sep, std::move(right)};
    }

    std::unique_ptr<Node> root_;
    size_t max_keys_;
    size_t entries_ = 0, height_ = 1;
};

}  // namespace adb

