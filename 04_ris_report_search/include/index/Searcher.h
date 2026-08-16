// ============================================================================
// Searcher.h — 检索执行器：分词 → 倒排召回 → 余弦相似度 TopK → 上下文摘要
//
// 评分模型（简历 RIS bullet 2）：
//   文档向量已 L2 归一化（构建期完成）；查询向量同样做 TF-IDF + L2 归一化，
//   余弦相似度 = 两归一化向量的点积。TopK 用小顶堆（size=k），O(n log k)。
// 摘要：取查询首个命中词在 conclusion 中的位置，截取前后各 24 字节上下文。
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "index/InvertedIndex.h"

namespace ris {
namespace index {

struct SearchHit {
    uint32_t doc_id;
    double score;      // 余弦相似度
    std::string report_id;
    std::string patient_id;
    std::string patient_name;
    std::string exam_date;
    std::string modality;
    std::string study_instance_uid; // 非空且在 pacs_db 存在时表示可跳转影像
    std::string snippet;            // 命中位置上下文摘要
};

class Searcher {
public:
    Searcher(const Tokenizer &tok) : tok_(tok) {}

    // 每次检索都从全局槽位取当前版本快照（索引切换后下一次查询即用新版本）
    std::vector<SearchHit> search(const std::string &query, size_t top_k) const;

private:
    static std::string make_snippet(const std::string &text, const std::string &term);

    const Tokenizer &tok_;
};

} // namespace index
} // namespace ris
