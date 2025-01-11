#pragma once

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <vector>
#include "spdlog/spdlog.h"
#include "src/type/tuple.h"
#include "src/type/value.h"


namespace neon::index
{

using neon::type::Row;
using neon::type::RowRef;
using neon::type::Value;

using KvEntry = std::pair<Value, RowRef>;

struct Node
{
    bool is_leaf_;
    Node * next_;
    std::vector<KvEntry> kv_;
    std::vector<Node *> childs_;
    explicit Node(bool is_leaf) : is_leaf_(is_leaf), next_(nullptr) { }
};

class BPTree;

class BpTreeIter
{
private:
    BPTree * tree_;
    Node * cur_;
    int idx_;

public:
    friend BPTree;
    BpTreeIter(BPTree * tree, Node * cur)
    {
        tree_ = tree;
        cur_ = cur;
        idx_ = 0;
    }

    bool is_valid() { return cur_ != nullptr; }

    void next()
    {
        const auto cur_kv_size = cur_->kv_.size();
        if (idx_ < cur_kv_size)
        {
            idx_++;
        }
        else
        {
            cur_ = cur_->next_;
            idx_ = 0;
        }
    }

    const std::pair<Value, RowRef> * kv_ref() { return &cur_->kv_[idx_]; }
};


class BPTree
{
public:
    // 只能是偶数
    static constexpr int Degree = 4;
    static constexpr int MaxKeySize = 2 * Degree - 1;
    Node * root_;

    explicit BPTree() { root_ = new Node(true); }
    ~BPTree();
    BpTreeIter iter()
    {
        Node * cur = root_;
        while (!cur->is_leaf_)
        {
            cur = cur->childs_[0];
        }
        return {this, cur};
    }

    BpTreeIter iter_with_begin(const Value & begin_value)
    {
        Node * cur = root_;
        while (!cur->is_leaf_)
        {
            int i = 0;
            while (i < cur->kv_.size() && begin_value > cur->kv_[i].first)
            {
                i++;
            }
            cur = cur->childs_[i];
        }
        int i = 0;
        while (cur != nullptr) {
            for (const auto& [key, row] : cur->kv_) {
                if (key >= begin_value) {
                    break;
                }
                i++;
            }
            cur = cur->next_;
        }
        auto iter = BpTreeIter{this, cur};
        iter.idx_ = i;
        return iter;
    }

    void insert(const Value & key, const RowRef & row)
    {
        if (root_->kv_.size() == MaxKeySize)
        {
            auto new_root = new Node(false);
            new_root->childs_.push_back(root_);
            split_child(new_root, 0, root_);
            root_ = new_root;
        }
        insert_non_full(root_, key, row);
    }

    [[nodiscard]] RowRef get(const Value & key) const
    {
        Node * current = root_;
        while (current != nullptr)
        {
            int i = 0;
            while (i < current->kv_.size() && key > current->kv_[i].first)
            {
                i++;
            }
            if (i < current->kv_.size() && key == current->kv_[i].first)
            {
                return current->kv_[i].second;
            }
            if (current->is_leaf_)
            {
                return nullptr;
            }
            current = current->childs_[i];
        }
        return nullptr;
    }

    bool remove(const Value & key)
    {
        if (root_ == nullptr)
        {
            return false;
        }
        const auto is_delete = remove(root_, key);
        if (root_->kv_.empty() && !root_->is_leaf_)
        {
            Node * tmp = root_;
            root_ = root_->childs_[0];
            delete tmp;
        }
        return is_delete;
    }

    [[nodiscard]] std::string to_string() const
    {
        std::string res;
        Node * cur = root_;
        while (!cur->is_leaf_)
        {
            cur = cur->childs_[0];
        }

        while (cur != nullptr)
        {
            for (const auto & [key, row] : cur->kv_)
            {
                res += std::format("Key: {}, Row: {}\n", key.to_val_string(), row->to_string());
            }
            cur = cur->next_;
        }
        return res;
    }

private:
    void insert_non_full(Node * node, const Value & key, const RowRef & row)
    {
        if (node->is_leaf_)
        {
            auto kv = std::make_pair(key, row);
            auto pos = std::upper_bound(node->kv_.begin(), node->kv_.end(), kv);
            SPDLOG_INFO("pos:{}", std::distance(node->kv_.begin(), pos));
            node->kv_.insert(pos, kv);
        }
        else
        {
            int i = node->kv_.size() - 1;
            while (i >= 0 && key < node->kv_[i].first)
            {
                i--;
            }
            i++;
            if (node->childs_[i]->kv_.size() == MaxKeySize)
            {
                split_child(node, i, node->childs_[i]);
                if (key > node->kv_[i].first)
                {
                    i++;
                }
            }
            insert_non_full(node->childs_[i], key, row);
        }
    }

    void split_child(Node * parent, int idx, Node * child)
    {
        auto new_child = new Node(child->is_leaf_);
        parent->childs_.insert(parent->childs_.begin() + idx + 1, new_child);
        parent->kv_.insert(parent->kv_.begin() + idx, child->kv_[Degree - 1]);

        new_child->kv_.assign(child->kv_.begin() + Degree, child->kv_.end());
        child->kv_.resize(Degree - 1);

        if (!child->is_leaf_)
        {
            new_child->childs_.assign(child->childs_.begin() + Degree, child->childs_.end());
            child->childs_.resize(Degree);
        }
        else
        {
            new_child->next_ = child->next_;
            child->next_ = new_child;
        }
    }

    bool remove(Node * node, const Value & key)
    {
        if (node->is_leaf_)
        {
            auto it = std::find_if(node->kv_.begin(), node->kv_.end(), [&key](const KvEntry & entry) { return entry.first == key; });
            if (it != node->kv_.end())
            {
                node->kv_.erase(it);
                return true;
            }
            return false;
        }
        else
        {
            auto it = std::lower_bound(
                node->kv_.begin(), node->kv_.end(), key, [](const KvEntry & entry, const Value & key) { return entry.first < key; });
            int idx = it - node->kv_.begin();
            if (idx < node->kv_.size() && node->kv_[idx].first == key)
            {
                if (node->childs_[idx]->kv_.size() >= Degree)
                {
                    Node * pre_node = node->childs_[idx];
                    while (!pre_node->is_leaf_)
                    {
                        pre_node = pre_node->childs_.back();
                    }
                    auto pred = pre_node->kv_.back();
                    node->kv_[idx] = pred;
                    return this->remove(node->childs_[idx], pred.first);
                }
                else if (node->childs_[idx + 1]->kv_.size() >= Degree)
                {
                    auto succ_node = node->childs_[idx + 1];
                    while (!succ_node->is_leaf_)
                    {
                        succ_node = succ_node->childs_.front();
                    }
                    auto succ = succ_node->kv_.front();
                    node->kv_[idx] = succ;
                    return remove(node->childs_[idx + 1], succ.first);
                }
                else
                {
                    merge(node, idx);
                    return remove(node->childs_[idx], key);
                }
            }
            else
            {
                if (node->childs_[idx]->kv_.size() < Degree)
                {
                    if (idx > 0 && node->childs_[idx - 1]->kv_.size() >= Degree)
                    {
                        borrow_from_prev(node, idx);
                    }
                    else if (idx < node->childs_.size() - 1 && node->childs_[idx + 1]->kv_.size() >= Degree)
                    {
                        borrow_from_next(node, idx);
                    }
                    else
                    {
                        if (idx < node->childs_.size() - 1)
                        {
                            merge(node, idx);
                        }
                        else
                        {
                            merge(node, idx - 1);
                        }
                    }
                }
                return remove(node->childs_[idx], key);
            }
        }
        return false;
    }

    void borrow_from_prev(Node * node, const int idx)
    {
        Node * child = node->childs_[idx];
        Node * sibling = node->childs_[idx - 1];

        child->kv_.insert(child->kv_.begin(), node->kv_[idx - 1]);
        node->kv_[idx - 1] = sibling->kv_.back();
        sibling->kv_.pop_back();

        if (!child->is_leaf_)
        {
            child->childs_.insert(child->childs_.begin(), sibling->childs_.back());
            sibling->childs_.pop_back();
        }
    }

    void borrow_from_next(Node * node, const int idx)
    {
        Node * child = node->childs_[idx];
        Node * sibling = node->childs_[idx + 1];

        child->kv_.push_back(node->kv_[idx]);
        node->kv_[idx] = sibling->kv_.front();
        sibling->kv_.erase(sibling->kv_.begin());

        if (!child->is_leaf_)
        {
            child->childs_.push_back(sibling->childs_.front());
            sibling->childs_.erase(sibling->childs_.begin());
        }
    }

    void merge(Node * node, const int idx)
    {
        Node * child = node->childs_[idx];
        Node * sibling = node->childs_[idx + 1];

        child->kv_.push_back(node->kv_[idx]);
        child->kv_.insert(child->kv_.end(), sibling->kv_.begin(), sibling->kv_.end());

        if (!child->is_leaf_)
        {
            child->childs_.insert(child->childs_.end(), sibling->childs_.begin(), sibling->childs_.end());
        }

        node->kv_.erase(node->kv_.begin() + idx);
        node->childs_.erase(node->childs_.begin() + idx + 1);

        delete sibling;
    }
};

}
