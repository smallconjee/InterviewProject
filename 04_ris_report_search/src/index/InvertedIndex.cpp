#include "index/InvertedIndex.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>

#include <cppjieba/Jieba.hpp>
#include <muduo/base/Logging.h>

namespace ris {
namespace index {

namespace {

std::shared_ptr<const IndexSnapshot> g_snapshot; // 全局索引槽位（原子读写）

} // namespace

// ---------- IndexHolder：原子读写 shared_ptr（C++11 free functions） ----------

std::shared_ptr<const IndexSnapshot> IndexHolder::load() {
    return std::atomic_load(&g_snapshot);
}

void IndexHolder::store(std::shared_ptr<const IndexSnapshot> snap) {
    std::atomic_store(&g_snapshot, snap);
}

// ---------- Tokenizer ----------

Tokenizer::Tokenizer(const std::string &dict_dir)
    : jieba_(new cppjieba::Jieba(dict_dir + "/jieba.dict.utf8",
                                 dict_dir + "/hmm_model.utf8",
                                 dict_dir + "/user.dict.utf8",
                                 dict_dir + "/idf.utf8",
                                 dict_dir + "/stop_words.utf8")) {
    // 停用词单独加载一份哈希集合：jieba 的 CutForSearch 不做停用词过滤
    std::ifstream in((dict_dir + "/stop_words.utf8").c_str());
    std::string w;
    while (std::getline(in, w)) {
        // 词典行可能带 \r
        if (!w.empty() && w[w.size() - 1] == '\r') w.erase(w.size() - 1);
        if (!w.empty()) stopwords_[w] = true;
    }
}

std::vector<std::string> Tokenizer::cut(const std::string &text) const {
    cppjieba::Jieba *j = reinterpret_cast<cppjieba::Jieba *>(jieba_);
    std::vector<std::string> words;
    // Mix 模式：先最大概率分词，再用 HMM 补新词（搜索引擎项目的标准选择）
    j->Cut(text, words, true);
    std::vector<std::string> out;
    out.reserve(words.size());
    for (size_t i = 0; i < words.size(); ++i) {
        const std::string &w = words[i];
        if (w.empty() || stopwords_.count(w) > 0) continue;
        // 单个 ASCII 字符/标点噪声太多，过滤（中文单字保留——医学术语如"痛"有语义）
        if (w.size() == 1 && (unsigned char)w[0] < 0x80) continue;
        out.push_back(w);
    }
    return out;
}

// ---------- 序列化 ----------

// TSV 转义：结论/所见含换行与制表符
std::string esc_field(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') out += "\\n";
        else if (s[i] == '\t') out += "\\t";
        else if (s[i] == '\\') out += "\\\\";
        else out += s[i];
    }
    return out;
}

std::string unesc_field(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            if (s[i + 1] == 'n') { out += '\n'; ++i; continue; }
            if (s[i + 1] == 't') { out += '\t'; ++i; continue; }
            if (s[i + 1] == '\\') { out += '\\'; ++i; continue; }
        }
        out += s[i];
    }
    return out;
}

bool write_snapshot(const std::shared_ptr<const IndexSnapshot> &snap, const std::string &dir) {
    // 简单可靠；目录名来自内部生成（版本号拼接），无注入风险
    std::string cmd = "mkdir -p " + dir;
    if (::system(cmd.c_str()) != 0) return false;

    std::ofstream meta((dir + "/meta.txt").c_str());
    meta << "version\t" << snap->version << "\n";
    meta << "docs\t" << snap->docs.size() << "\n";

    std::ofstream docs((dir + "/docs.tsv").c_str());
    for (size_t i = 0; i < snap->docs.size(); ++i) {
        const report::ReportRecord &r = snap->docs[i];
        docs << i << "\t" << esc_field(r.report_id) << "\t" << esc_field(r.patient_id) << "\t"
             << esc_field(r.patient_name) << "\t" << esc_field(r.exam_date) << "\t"
             << esc_field(r.modality) << "\t" << esc_field(r.exam_part) << "\t"
             << esc_field(r.conclusion) << "\t" << esc_field(r.description) << "\t"
             << esc_field(r.study_instance_uid) << "\n";
    }

    std::ofstream post((dir + "/postings.tsv").c_str());
    for (std::unordered_map<std::string, std::vector<PostEntry>>::const_iterator it =
             snap->inverted.begin();
         it != snap->inverted.end(); ++it) {
        post << it->first;
        for (size_t k = 0; k < it->second.size(); ++k) {
            char buf[48];
            // %.6f 足够：权重只用于排序比较，不参与可逆计算
            std::snprintf(buf, sizeof(buf), "\t%u,%.6f", it->second[k].doc_id,
                          it->second[k].weight);
            post << buf;
        }
        post << "\n";
    }
    return meta.good() && docs.good() && post.good();
}

// ---------- 构建 ----------

int build_index(const std::string &reports_dir, const std::string &index_dir,
                const Tokenizer &tok, std::string &err) {
    report::DirScanResult scan = report::parse_report_dir(reports_dir);

    // report_id 去重：重复记录跳过（数据源重发的常见形态）
    std::map<std::string, bool> seen;
    std::vector<report::ReportRecord> deduped;
    int dup_count = 0;
    for (size_t i = 0; i < scan.records.size(); ++i) {
        if (scan.records[i].report_id.empty() || seen.count(scan.records[i].report_id)) {
            ++dup_count;
            continue;
        }
        seen[scan.records[i].report_id] = true;
        deduped.push_back(scan.records[i]);
    }
    if (deduped.empty()) {
        err = "没有可用报告（解析成功 0 条，坏文件 " + std::to_string(scan.bad.size()) + " 个）";
        return -1;
    }

    // ---- TF 统计：conclusion 权重 2 份（主字段）、description 1 份 ----
    const size_t N = deduped.size();
    std::vector<std::map<std::string, double>> tf(N);
    std::unordered_map<std::string, uint32_t> df;
    for (size_t i = 0; i < N; ++i) {
        std::map<std::string, double> &m = tf[i];
        std::vector<std::string> c = tok.cut(deduped[i].conclusion);
        for (size_t k = 0; k < c.size(); ++k) m[c[k]] += 2.0;
        std::vector<std::string> d = tok.cut(deduped[i].description);
        for (size_t k = 0; k < d.size(); ++k) m[d[k]] += 1.0;
        for (std::map<std::string, double>::iterator it = m.begin(); it != m.end(); ++it) {
            df[it->first] += 1;
        }
    }

    // ---- TF-IDF + 每文档 L2 归一化（余弦相似度的前提）----
    std::shared_ptr<IndexSnapshot> snap(new IndexSnapshot());
    snap->docs = deduped;
    snap->term_df = df;
    std::vector<double> norms(N, 0.0);
    for (size_t i = 0; i < N; ++i) {
        for (std::map<std::string, double>::iterator it = tf[i].begin(); it != tf[i].end(); ++it) {
            double idf = std::log((double)N / (double)df[it->first]);
            double w = it->second * idf;
            tf[i][it->first] = w;
            norms[i] += w * w;
        }
        norms[i] = std::sqrt(norms[i]);
    }
    for (size_t i = 0; i < N; ++i) {
        uint32_t doc_id = static_cast<uint32_t>(i);
        for (std::map<std::string, double>::iterator it = tf[i].begin(); it != tf[i].end(); ++it) {
            PostEntry e;
            e.doc_id = doc_id;
            e.weight = norms[i] > 0.0 ? it->second / norms[i] : 0.0;
            snap->inverted[it->first].push_back(e);
        }
    }

    // ---- 版本号 = 现有最大版本 + 1 ----
    snap->version = load_latest(index_dir)->version + 1;
    std::string dir = index_dir + "/v" + std::to_string(snap->version);
    if (!write_snapshot(snap, dir)) {
        err = "写索引目录失败: " + dir;
        return -1;
    }

    LOG_INFO << "[RIS Search] 索引构建完成: v" << snap->version << " 文档 " << N
             << " 词项 " << snap->inverted.size() << "（重复 " << dup_count << " 坏文件 "
             << scan.bad.size() << "）";
    // 构建后立即切换全局槽位（--build-index 与服务同进程时立即生效）
    IndexHolder::store(snap);
    return snap->version;
}

// ---------- 加载 ----------

std::shared_ptr<const IndexSnapshot> load_latest(const std::string &index_dir) {
    // 扫描 v<N> 目录取最大版本
    DIR *d = ::opendir(index_dir.c_str());
    if (d == NULL) {
        return std::shared_ptr<const IndexSnapshot>(new IndexSnapshot()); // version=0
    }
    int best = 0;
    struct dirent *ent;
    while ((ent = ::readdir(d)) != NULL) {
        if (std::strncmp(ent->d_name, "v", 1) == 0) {
            int v = std::atoi(ent->d_name + 1);
            if (v > best) best = v;
        }
    }
    ::closedir(d);
    if (best == 0) {
        return std::shared_ptr<const IndexSnapshot>(new IndexSnapshot());
    }

    std::string dir = index_dir + "/v" + std::to_string(best);
    std::shared_ptr<IndexSnapshot> snap(new IndexSnapshot());
    snap->version = best;

    std::ifstream docs((dir + "/docs.tsv").c_str());
    std::string line;
    while (std::getline(docs, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        std::istringstream ss(line);
        report::ReportRecord r;
        std::string id;
        std::getline(ss, id, '\t');
        std::getline(ss, r.report_id, '\t');
        std::getline(ss, r.patient_id, '\t');
        std::getline(ss, r.patient_name, '\t');
        std::getline(ss, r.exam_date, '\t');
        std::getline(ss, r.modality, '\t');
        std::getline(ss, r.exam_part, '\t');
        std::getline(ss, r.conclusion, '\t');
        std::getline(ss, r.description, '\t');
        std::getline(ss, r.study_instance_uid, '\t');
        r.report_id = unesc_field(r.report_id);
        r.patient_id = unesc_field(r.patient_id);
        r.patient_name = unesc_field(r.patient_name);
        r.conclusion = unesc_field(r.conclusion);
        r.description = unesc_field(r.description);
        r.study_instance_uid = unesc_field(r.study_instance_uid);
        snap->docs.push_back(r);
    }

    std::ifstream post((dir + "/postings.tsv").c_str());
    while (std::getline(post, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        std::istringstream ss(line);
        std::string term;
        if (!std::getline(ss, term, '\t')) continue;
        std::vector<PostEntry> &list = snap->inverted[term];
        std::string item;
        while (std::getline(ss, item, '\t')) {
            size_t comma = item.find(',');
            if (comma == std::string::npos) continue;
            PostEntry e;
            e.doc_id = static_cast<uint32_t>(std::strtoul(item.substr(0, comma).c_str(), NULL, 10));
            e.weight = std::atof(item.c_str() + comma + 1);
            list.push_back(e);
        }
    }

    // term_df 从倒排表重建
    for (std::unordered_map<std::string, std::vector<PostEntry>>::iterator it =
             snap->inverted.begin();
         it != snap->inverted.end(); ++it) {
        snap->term_df[it->first] = static_cast<uint32_t>(it->second.size());
    }
    return snap;
}

} // namespace index
} // namespace ris
