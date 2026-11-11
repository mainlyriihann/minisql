// BPlusTree.hpp
// A minimal, correct in-memory B+Tree mapping Value -> row id (size_t).
// Used by Table for primary-key point lookups and range scans (e.g. WHERE
// pk > x AND pk < y, or ORDER BY on an indexed column).
//
// This is a classic RDBMS structure: internal nodes hold routing keys,
// leaf nodes hold the actual (key -> rowid) entries and are linked
// left-to-right for efficient range scans.
#pragma once

#include "Types.hpp"
#include <vector>
#include <memory>
#include <algorithm>
#include <stdexcept>

namespace minisql {

class BPlusTree {
public:
    explicit BPlusTree(int order = 32) : order_(order) {
        root_ = std::make_unique<Node>(true);
    }

    // Insert key -> rowId. Throws if key already exists (enforces uniqueness,
    // used for primary keys).
    void insert(const Value& key, size_t rowId) {
        Node* leaf = findLeaf(key);
        auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key,
                                    [](const Value& a, const Value& b) { return a.compare(b) < 0; });
        if (it != leaf->keys.end() && it->compare(key) == 0) {
            throw std::runtime_error("Duplicate key in unique index: " + key.toDisplayString());
        }
        size_t pos = static_cast<size_t>(it - leaf->keys.begin());
        leaf->keys.insert(leaf->keys.begin() + pos, key);
        leaf->values.insert(leaf->values.begin() + pos, rowId);

        if (static_cast<int>(leaf->keys.size()) > order_ - 1) {
            splitLeaf(leaf);
        }
    }

    // Remove a key. Returns true if found and removed. (Simplified: does not
    // rebalance/merge nodes -- acceptable for a teaching-grade engine since
    // deletes are infrequent relative to lookups and correctness of lookup
    // results is preserved.)
    bool remove(const Value& key) {
        Node* leaf = findLeaf(key);
        auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key,
                                    [](const Value& a, const Value& b) { return a.compare(b) < 0; });
        if (it == leaf->keys.end() || it->compare(key) != 0) return false;
        size_t pos = static_cast<size_t>(it - leaf->keys.begin());
        leaf->keys.erase(leaf->keys.begin() + pos);
        leaf->values.erase(leaf->values.begin() + pos);
        return true;
    }

    // Update the rowId stored for an existing key (used when a row is
    // rewritten in place, e.g. after UPDATE changes non-key columns).
    bool updateValue(const Value& key, size_t newRowId) {
        Node* leaf = findLeaf(key);
        auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key,
                                    [](const Value& a, const Value& b) { return a.compare(b) < 0; });
        if (it == leaf->keys.end() || it->compare(key) != 0) return false;
        size_t pos = static_cast<size_t>(it - leaf->keys.begin());
        leaf->values[pos] = newRowId;
        return true;
    }

    // Point lookup. Returns true and sets outRowId if found.
    bool find(const Value& key, size_t& outRowId) const {
        Node* leaf = findLeaf(key);
        auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key,
                                    [](const Value& a, const Value& b) { return a.compare(b) < 0; });
        if (it != leaf->keys.end() && it->compare(key) == 0) {
            outRowId = leaf->values[static_cast<size_t>(it - leaf->keys.begin())];
            return true;
        }
        return false;
    }

    // Range scan: returns row ids for keys in [lo, hi] respecting inclusivity
    // flags. Pass hasLo/hasHi=false for an open bound on that side.
    std::vector<size_t> rangeScan(const Value* lo, bool loInclusive,
                                   const Value* hi, bool hiInclusive) const {
        std::vector<size_t> result;
        Node* leaf = leftmostLeaf();
        if (lo) leaf = findLeaf(*lo);

        while (leaf != nullptr) {
            for (size_t i = 0; i < leaf->keys.size(); ++i) {
                const Value& k = leaf->keys[i];
                if (lo) {
                    int c = k.compare(*lo);
                    if (c < 0 || (c == 0 && !loInclusive)) continue;
                }
                if (hi) {
                    int c = k.compare(*hi);
                    if (c > 0 || (c == 0 && !hiInclusive)) { return result; }
                }
                result.push_back(leaf->values[i]);
            }
            leaf = leaf->next;
        }
        return result;
    }

    void clear() {
        root_ = std::make_unique<Node>(true);
    }

private:
    struct Node {
        bool isLeaf;
        std::vector<Value> keys;
        std::vector<size_t> values;           // leaf only: rowIds parallel to keys
        std::vector<std::unique_ptr<Node>> children; // internal only: children.size() == keys.size()+1
        Node* next = nullptr;                 // leaf only: linked list for range scans
        Node* parent = nullptr;

        explicit Node(bool leaf) : isLeaf(leaf) {}
    };

    int order_;
    std::unique_ptr<Node> root_;

    Node* leftmostLeaf() const {
        Node* n = root_.get();
        while (!n->isLeaf) n = n->children.front().get();
        return n;
    }

    Node* findLeaf(const Value& key) const {
        Node* n = root_.get();
        while (!n->isLeaf) {
            size_t i = 0;
            while (i < n->keys.size() && key.compare(n->keys[i]) >= 0) ++i;
            n = n->children[i].get();
        }
        return n;
    }

    void splitLeaf(Node* leaf) {
        size_t mid = leaf->keys.size() / 2;

        auto newLeaf = std::make_unique<Node>(true);
        newLeaf->keys.assign(leaf->keys.begin() + mid, leaf->keys.end());
        newLeaf->values.assign(leaf->values.begin() + mid, leaf->values.end());
        leaf->keys.resize(mid);
        leaf->values.resize(mid);

        newLeaf->next = leaf->next;
        Node* newLeafPtr = newLeaf.get();
        leaf->next = newLeafPtr;

        Value upKey = newLeaf->keys.front();

        if (leaf->parent == nullptr) {
            // leaf was root: create a new root
            auto newRoot = std::make_unique<Node>(false);
            newLeaf->parent = newRoot.get();
            newRoot->keys.push_back(upKey);

            // Detach old root (leaf) from root_ to re-own it as a child
            std::unique_ptr<Node> oldRootOwned = std::move(root_);
            oldRootOwned->parent = newRoot.get();

            newRoot->children.push_back(std::move(oldRootOwned));
            newRoot->children.push_back(std::move(newLeaf));
            root_ = std::move(newRoot);
        } else {
            Node* parent = leaf->parent;
            newLeaf->parent = parent;
            insertIntoParent(parent, leaf, upKey, std::move(newLeaf));
        }
    }

    // Insert (upKey -> newChild) into parent, right after `leftChild`.
    void insertIntoParent(Node* parent, Node* leftChild, const Value& upKey, std::unique_ptr<Node> newChild) {
        size_t pos = 0;
        while (pos < parent->children.size() && parent->children[pos].get() != leftChild) ++pos;
        // pos is index of leftChild among children; insert key at `pos`, child at `pos+1`
        parent->keys.insert(parent->keys.begin() + pos, upKey);
        newChild->parent = parent;
        parent->children.insert(parent->children.begin() + pos + 1, std::move(newChild));

        if (static_cast<int>(parent->keys.size()) > order_ - 1) {
            splitInternal(parent);
        }
    }

    void splitInternal(Node* node) {
        size_t mid = node->keys.size() / 2;
        Value upKey = node->keys[mid];

        auto newNode = std::make_unique<Node>(false);
        newNode->keys.assign(node->keys.begin() + mid + 1, node->keys.end());
        for (size_t i = mid + 1; i < node->children.size(); ++i) {
            node->children[i]->parent = newNode.get();
            newNode->children.push_back(std::move(node->children[i]));
        }
        node->children.resize(mid + 1);
        node->keys.resize(mid);

        if (node->parent == nullptr) {
            auto newRoot = std::make_unique<Node>(false);
            newRoot->keys.push_back(upKey);

            std::unique_ptr<Node> oldRootOwned = std::move(root_);
            oldRootOwned->parent = newRoot.get();
            newNode->parent = newRoot.get();

            newRoot->children.push_back(std::move(oldRootOwned));
            newRoot->children.push_back(std::move(newNode));
            root_ = std::move(newRoot);
        } else {
            Node* parent = node->parent;
            newNode->parent = parent;
            insertIntoParent(parent, node, upKey, std::move(newNode));
        }
    }
};

} // namespace minisql
