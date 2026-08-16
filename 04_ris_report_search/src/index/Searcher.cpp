#include "index/Searcher.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace ris {
namespace index {

namespace {

// 小顶堆的比较器：堆顶是 TopK 中得分最低的，便于淘汰
struct HitLess {
    bool operator()(const SearchHit &a, const SearchHit &b) const {
        return a.score > b.score;
    }
};

} // namespace

std::string Searcher::make_snippet(const std::string &text, const std::string &term) {
    if (text.empty() || term.empty()) return "";
    size_t pos = text.find(term);
    if (pos == std::string::npos) {
        // 未命中也返回开头摘要（结果列表需要上下文）；同样要对齐 UTF-8 边界
        size_t len = std::min<size_t>(text.size(), 48);
        while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80) {
            --len; // 退回到字符起点
        }
        std::string s = text.substr(0, len);
        if (text.size() > len) s += "...";
        return s;
    }
    // 前后各约 24 字节；按字节截断可能切碎 UTF-8 多字节字符，
    // 向前/向后对齐到字符边界（后续字节高两位是 10 的是续字节）
    size_t begin = pos > 24 ? pos - 24 : 0;
    while (begin > 0 && (static_cast<unsigned char>(text[begin]) & 0xC0) == 0x80) {
        --begin;
    }
    size_t len = std::min(text.size() - begin, term.size() + 48);
    while (begin + len < text.size() &&
           (static_cast<unsigned char>(text[begin + len]) & 0xC0) == 0x80) {
        ++len; // 尾部补齐被切断的字符
    }
    std::string s = text.substr(begin, len);
    if (begin > 0) s = "..." + s;
    if (begin + len < text.size()) s = s + "...";
    return s;
}

std::vector<SearchHit> Searcher::search(const std::string &query, size_t top_k) const {
    std::vector<SearchHit> out;
    std::shared_ptr<const IndexSnapshot> snap = IndexHolder::load();
    if (!snap || snap->docs.empty() || top_k == 0) return out;

    // 查询向量：TF *（索引同源 IDF）后 L2 归一化
    std::vector<std::string> terms = tok_.cut(query);
    if (terms.empty()) return out;
    std::unordered_map<std::string, double> qtf;
    for (size_t i = 0; i < terms.size(); ++i) {
        qtf[terms[i]] += 1.0;
    }
    const double N = static_cast<double>(snap->docs.size());
    double qnorm = 0.0;
    std::unordered_map<std::string, double> qw;
    for (std::unordered_map<std::string, double>::iterator it = qtf.begin(); it != qtf.end();
         ++it) {
        std::unordered_map<std::string, std::vector<PostEntry>>::const_iterator inv =
            snap->inverted.find(it->first);
        if (inv == snap->inverted.end()) continue; // 词不在索引里：无召回贡献
        double idf = std::log(N / static_cast<double>(inv->second.size()));
        double w = it->second * idf;
        if (w > 0.0) {
            qw[it->first] = w;
            qnorm += w * w;
        }
    }
    if (qw.empty()) return out; // 查询词全部不在索引中
    qnorm = std::sqrt(qnorm);

    // 点积累加（文档向量已归一化）→ doc -> score
    std::unordered_map<uint32_t, double> scores;
    for (std::unordered_map<std::string, double>::iterator it = qw.begin(); it != qw.end();
         ++it) {
        const std::vector<PostEntry> &posts = snap->inverted.find(it->first)->second;
        double contribution = it->second / qnorm;
        for (size_t k = 0; k < posts.size(); ++k) {
            scores[posts[k].doc_id] += posts[k].weight * contribution;
        }
    }

    // TopK 小顶堆
    std::priority_queue<SearchHit, std::vector<SearchHit>, HitLess> heap;
    for (std::unordered_map<uint32_t, double>::iterator it = scores.begin();
         it != scores.end(); ++it) {
        SearchHit h;
        h.doc_id = it->first;
        h.score = it->second;
        if (heap.size() < top_k) {
            heap.push(h);
        } else if (h.score > heap.top().score) {
            heap.pop();
            heap.push(h);
        }
    }
    out.reserve(heap.size());
    while (!heap.empty()) {
        out.push_back(heap.top());
        heap.pop();
    }
    std::sort(out.begin(), out.end(), HitLess()); // 降序

    // 补充元数据与摘要
    for (size_t i = 0; i < out.size(); ++i) {
        const report::ReportRecord &r = snap->docs[out[i].doc_id];
        out[i].report_id = r.report_id;
        out[i].patient_id = r.patient_id;
        out[i].patient_name = r.patient_name;
        out[i].exam_date = r.exam_date;
        out[i].modality = r.modality;
        out[i].study_instance_uid = r.study_instance_uid;
        out[i].snippet = make_snippet(r.conclusion, qw.begin()->first);
    }
    return out;
}

} // namespace index
} // namespace ris
