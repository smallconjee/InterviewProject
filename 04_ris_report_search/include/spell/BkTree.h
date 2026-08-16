// ============================================================================
// BkTree.h — BK-tree 拼写纠错（简历 RIS bullet 4）
//
// 为什么 BK-tree：词典 1 万词时，暴力全量编辑距离是 1 万次 O(L²) DP；
// BK-tree 按编辑距离分桶剪枝，只访问与查询距离可能 ≤N 的子树，
// 典型剪枝率 90%+——"避免查询时遍历完整词典"的简历原话。
// 编辑距离按 Unicode 码点计算（utfcpp 切码点）：中文一字一单位，
// "肺节结"→"肺结节"距离 2；按字节算会是 3（一字 3 字节），中文纠错失效。
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ris {
namespace spell {

struct SuggestItem {
    std::string term;
    int distance;
    uint32_t freq; // 词频（文档频率）：同距离时优先高频词
};

class BkTree {
public:
    BkTree() : root_(NULL), size_(0) {}

    // 不可拷贝（树节点裸指针）；服务进程内单实例常驻
    BkTree(const BkTree &) = delete;
    BkTree &operator=(const BkTree &) = delete;

    void insert(const std::string &term, uint32_t freq);

    // 编辑距离 <= max_dist 的候选，按 (distance, -freq) 取前 k 个
    std::vector<SuggestItem> suggest(const std::string &query, int max_dist, size_t k) const;

    size_t size() const { return size_; }

private:
    struct Node {
        std::string term;
        uint32_t freq;
        std::map<int, Node *> children; // 边权 = 父词与子词的编辑距离
        Node(const std::string &t, uint32_t f) : term(t), freq(f) {}
    };

    static int edit_distance(const std::string &a, const std::string &b);
    void collect(const Node *node, const std::string &query, int max_dist, size_t k,
                 std::vector<SuggestItem> &out) const;

    Node *root_;
    size_t size_;
};

} // namespace spell
} // namespace ris
