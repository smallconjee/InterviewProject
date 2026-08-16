// ============================================================================
// InvertedIndex.h — 不可变倒排索引 + 版本化切换（简历 RIS bullet 2）
//
// IndexSnapshot 是纯只读结构：构建完成后永不修改，通过 shared_ptr + 原子操作
// 整体替换（双缓冲思想的现代形态）——检索线程要么用旧版本查完，要么用新版本
// 查完，不存在"半个索引"；旧版本由最后一个使用者析构（引用计数自动回收）。
// 索引落盘为版本目录（v1/v2/...），服务启动加载最大版本，重建即生成新版本。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "report/ReportParser.h"

namespace ris {
namespace index {

// 倒排表项：doc_id + 该词在该文档的 L2 归一化 TF-IDF 权重
struct PostEntry {
    uint32_t doc_id;
    double weight;
};

struct IndexSnapshot {
    int version = 0;
    std::vector<report::ReportRecord> docs;                    // doc_id -> 报告
    std::unordered_map<std::string, std::vector<PostEntry>> inverted; // term -> 倒排表
    // 词 -> 文档频率（BK-tree 纠错候选的优先级参考）
    std::unordered_map<std::string, uint32_t> term_df;
};

// 全局索引槽位：atomic free functions 操作 shared_ptr（C++11 标准设施）
class IndexHolder {
public:
    static std::shared_ptr<const IndexSnapshot> load();
    static void store(std::shared_ptr<const IndexSnapshot> snap);
};

// 分词器封装：cppjieba(Mix) + 停用词过滤；索引构建与查询共用同一实现
// （分词不一致是检索召回劣化的常见根因，收敛到一处）
class Tokenizer {
public:
    // dict_dir: cppjieba 词典目录（如 /usr/local/dict）
    Tokenizer(const std::string &dict_dir);

    // 返回过滤停用词后的词序列；dedup=false 保留重复（TF 统计用）
    std::vector<std::string> cut(const std::string &text) const;

private:
    void *jieba_; // cppjieba::Jieba*（头文件不暴露，缩短编译传染）
    std::unordered_map<std::string, bool> stopwords_;
};

// 离线构建：解析 reports_dir 全部报告 → 去重(report_id) → 分词 → TF-IDF 权重
// 归一化 → 序列化到 index_dir/v<N+1>/。返回新版本号；err 带失败原因。
int build_index(const std::string &reports_dir, const std::string &index_dir,
                const Tokenizer &tok, std::string &err);

// 加载 index_dir 下最大版本；无索引返回空 snapshot（version=0）
std::shared_ptr<const IndexSnapshot> load_latest(const std::string &index_dir);

} // namespace index
} // namespace ris
