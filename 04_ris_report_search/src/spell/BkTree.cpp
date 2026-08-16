#include "spell/BkTree.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <utfcpp/utf8.h>

namespace ris {
namespace spell {

// 编辑距离按 Unicode 码点计算（不是字节）：
// 一个汉字 3 字节，按字节算"肺节结"→"肺结节"距离 3，按码点算只有 2（交换=两次替换）。
// utfcpp 负责把 UTF-8 切成码点序列，DP 在码点维度进行。
int BkTree::edit_distance(const std::string &a, const std::string &b) {
    std::vector<uint32_t> va, vb;
    try {
        utf8::utf8to32(a.begin(), a.end(), std::back_inserter(va));
        utf8::utf8to32(b.begin(), b.end(), std::back_inserter(vb));
    } catch (const utf8::exception &) {
        return 64; // 非法 UTF-8：给一个大距离，等效于不参与召回
    }
    const int n = static_cast<int>(va.size());
    const int m = static_cast<int>(vb.size());
    if (n == 0) return m;
    if (m == 0) return n;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        for (int j = 1; j <= m; ++j) {
            int cost = (va[i - 1] == vb[j - 1]) ? 0 : 1;
            int best = prev[j] + 1;                                  // 删除
            if (cur[j - 1] + 1 < best) best = cur[j - 1] + 1;        // 插入
            if (prev[j - 1] + cost < best) best = prev[j - 1] + cost; // 替换
            cur[j] = best;
        }
        prev.swap(cur);
    }
    return prev[m];
}

void BkTree::insert(const std::string &term, uint32_t freq) {
    if (term.empty()) return;
    if (root_ == NULL) {
        root_ = new Node(term, freq);
        size_ = 1;
        return;
    }
    Node *cur = root_;
    for (;;) {
        int d = edit_distance(term, cur->term);
        if (d == 0) {
            cur->freq += freq; // 同词合并词频
            return;
        }
        std::map<int, Node *>::iterator it = cur->children.find(d);
        if (it == cur->children.end()) {
            cur->children[d] = new Node(term, freq);
            ++size_;
            return;
        }
        cur = it->second;
    }
}

void BkTree::collect(const Node *node, const std::string &query, int max_dist, size_t k,
                     std::vector<SuggestItem> &out) const {
    int d = edit_distance(query, node->term);
    if (d <= max_dist) {
        SuggestItem item;
        item.term = node->term;
        item.distance = d;
        item.freq = node->freq;
        out.push_back(item);
        if (out.size() > k * 4) {
            // 候选过多时先裁剪一轮，控制内存（k 通常 <= 10）
            std::sort(out.begin(), out.end(), [](const SuggestItem &x, const SuggestItem &y) {
                if (x.distance != y.distance) return x.distance < y.distance;
                return x.freq > y.freq;
            });
            if (out.size() > k) out.resize(k);
        }
    }
    // 剪枝核心：只有边权落在 [d-max_dist, d+max_dist] 的子树才可能有 ≤max_dist 的词
    for (std::map<int, Node *>::const_iterator it = node->children.begin();
         it != node->children.end(); ++it) {
        if (it->first >= d - max_dist && it->first <= d + max_dist) {
            collect(it->second, query, max_dist, k, out);
        }
    }
}

std::vector<SuggestItem> BkTree::suggest(const std::string &query, int max_dist,
                                         size_t k) const {
    std::vector<SuggestItem> out;
    if (root_ == NULL || query.empty()) return out;
    collect(root_, query, max_dist, k, out);
    std::sort(out.begin(), out.end(), [](const SuggestItem &x, const SuggestItem &y) {
        if (x.distance != y.distance) return x.distance < y.distance;
        return x.freq > y.freq;
    });
    if (out.size() > k) out.resize(k);
    return out;
}

} // namespace spell
} // namespace ris
