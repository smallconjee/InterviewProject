// ============================================================================
// LruCache.h — 线程安全 LRU（简历 RIS bullet 5 的一级缓存）
//
// 实现：mutex + list（双向链表维护访问序）+ unordered_map（key → 链表节点）。
// get 命中把节点摘到队头（O(1)），put 满则淘汰队尾（O(1)）。
// 为什么自写不引 lru-cache 库：~80 行核心逻辑，是面试高频手写题本体。
// ============================================================================
#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ris {
namespace cache {

class LruCache {
public:
    typedef std::string Key;
    typedef std::string Value;

    explicit LruCache(size_t capacity) : capacity_(capacity) {}

    // 命中返回 true 并写出值；LRU 语义：命中节点移动到队头
    bool get(const Key &key, Value &out) {
        std::lock_guard<std::mutex> lk(mtx_);
        std::unordered_map<Key, ListIt>::iterator it = index_.find(key);
        if (it == index_.end()) {
            return false;
        }
        items_.splice(items_.begin(), items_, it->second); // O(1) 摘到队头
        out = it->second->second;
        return true;
    }

    void put(const Key &key, const Value &value) {
        std::lock_guard<std::mutex> lk(mtx_);
        std::unordered_map<Key, ListIt>::iterator it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = value;
            items_.splice(items_.begin(), items_, it->second);
            return;
        }
        if (capacity_ == 0) return;
        if (index_.size() >= capacity_) {
            // 淘汰队尾（最久未使用）；先删索引再删节点，防止迭代器悬垂
            index_.erase(items_.back().first);
            items_.pop_back();
        }
        items_.push_front(std::make_pair(key, value));
        index_[key] = items_.begin();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return index_.size();
    }

private:
    typedef std::list<std::pair<Key, Value>>::iterator ListIt; // C++11 需先定义类型再用于声明

    const size_t capacity_;
    mutable std::mutex mtx_;
    std::list<std::pair<Key, Value>> items_;     // 队头 = 最近使用
    std::unordered_map<Key, ListIt> index_;
};

} // namespace cache
} // namespace ris
