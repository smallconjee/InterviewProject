// ============================================================================
// main.cpp — RIS 报告检索服务入口
//
// 两种运行模式：
//   ./ris_report_search --build-index <报告目录>   离线构建索引（生成新版本目录）
//   ./ris_report_search                            在线服务（加载最新索引 + muduo）
//
// 在线请求处理（TLV 协议，4 IO 线程 Reactor）：
//   0x01 SEARCH  query → L1 LRU → L2 Redis(版本化键) → 倒排+TF-IDF 余弦 TopK → JSON
//   0x02 SUGGEST query → BK-tree 编辑距离 ≤2 召回 Top5
//   0x7F ERROR   非法 type / 超长帧 / 空查询
// 报告-影像联动：结果含 study_instance_uid 且在 pacs_db.study 存在时置 has_image
// ============================================================================
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <mysql/mysql.h>

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/TcpConnection.h>
#include <muduo/net/TcpServer.h>

#include "cache/LruCache.h"
#include "cache/RedisCache.h"
#include "common/RisConfig.h"
#include "index/InvertedIndex.h"
#include "index/Searcher.h"
#include "protocol/TlvCodec.h"
#include "spell/BkTree.h"

// ----------------------------------------------------------------------------
// pacs_db 关联查询：报告里的 study_uid 是否有归档影像（跨子系统联动点）
// 单连接 + 互斥：关联查询频率 = 检索命中数，量级低；演进点：读连接池
// ----------------------------------------------------------------------------
class StudyLink {
public:
    StudyLink(const ris::common::RisConfig &cfg) : cfg_(cfg), conn_(NULL) {}

    ~StudyLink() {
        if (conn_ != NULL) ::mysql_close(conn_);
    }

    bool exists(const std::string &study_uid) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!ensure()) return false;
        char sql[512];
        // study_uid 来自我们自己解析的 XML（受控输入），长度校验后直接拼接
        if (study_uid.size() > 128) return false;
        std::snprintf(sql, sizeof(sql),
                      "SELECT 1 FROM study WHERE study_instance_uid='%.128s'",
                      study_uid.c_str());
        if (::mysql_query(conn_, sql) != 0) {
            LOG_WARN << "[RIS Search] 关联查询失败: " << ::mysql_error(conn_);
            return false;
        }
        MYSQL_RES *rs = ::mysql_store_result(conn_);
        bool found = (rs != NULL && ::mysql_num_rows(rs) > 0);
        if (rs != NULL) ::mysql_free_result(rs);
        return found;
    }

private:
    bool ensure() {
        if (conn_ != NULL) return true;
        conn_ = ::mysql_init(NULL);
        if (conn_ == NULL) return false;
        unsigned int timeout = 2;
        ::mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        if (::mysql_real_connect(conn_, cfg_.mysql_host.c_str(), cfg_.mysql_user.c_str(),
                                 cfg_.mysql_password.c_str(), "pacs_db", cfg_.mysql_port,
                                 NULL, 0) == NULL) {
            LOG_WARN << "[RIS Search] pacs_db 连接失败，has_image 降级为 false: "
                     << ::mysql_error(conn_);
            ::mysql_close(conn_);
            conn_ = NULL;
            return false;
        }
        return true;
    }

    ris::common::RisConfig cfg_;
    MYSQL *conn_;
    std::mutex mtx_;
};

// ----------------------------------------------------------------------------
// 检索服务器：持有全部组件；连接级状态（TLV 解码器）按连接名挂表
// ----------------------------------------------------------------------------
class RISSearchServer {
public:
    RISSearchServer(muduo::net::EventLoop *loop, const muduo::net::InetAddress &addr,
                    const ris::common::RisConfig &cfg)
        : cfg_(cfg), server_(loop, addr, "RISSearchServer"),
          tok_(new ris::index::Tokenizer(cfg.dict_dir)),
          searcher_(*tok_), lru_(cfg.lru_capacity), redis_(cfg.redis_host, cfg.redis_port),
          link_(cfg) {
        server_.setConnectionCallback(
            std::bind(&RISSearchServer::onConnection, this, std::placeholders::_1));
        server_.setMessageCallback(
            std::bind(&RISSearchServer::onMessage, this, std::placeholders::_1,
                      std::placeholders::_2, std::placeholders::_3));
        server_.setThreadNum(cfg_.io_threads); // sub-loop 数可配置：默认 4，压测后可调
    }

    void start() { server_.start(); }

private:
    void onConnection(const muduo::net::TcpConnectionPtr &conn) {
        if (conn->connected()) {
            // 每连接一个增量解码器：半帧状态留在各自的缓冲里
            std::lock_guard<std::mutex> lk(decoders_mtx_);
            decoders_[conn->name()] = std::make_shared<ris::protocol::TlvDecoder>();
            LOG_INFO << "[RIS Search] 新连接: " << conn->peerAddress().toIpPort()
                     << " (" << conn->name() << ")";
        } else {
            std::lock_guard<std::mutex> lk(decoders_mtx_);
            decoders_.erase(conn->name());
            LOG_INFO << "[RIS Search] 连接断开: " << conn->peerAddress().toIpPort();
        }
    }

    void onMessage(const muduo::net::TcpConnectionPtr &conn, muduo::net::Buffer *buf,
                   muduo::Timestamp) {
        std::shared_ptr<ris::protocol::TlvDecoder> dec;
        {
            std::lock_guard<std::mutex> lk(decoders_mtx_);
            std::map<std::string, std::shared_ptr<ris::protocol::TlvDecoder>>::iterator it =
                decoders_.find(conn->name());
            if (it == decoders_.end()) return;
            dec = it->second;
        }
        dec->feed(buf->retrieveAllAsString());

        ris::protocol::Frame frame;
        for (;;) {
            ris::protocol::DecodeStatus st = dec->decode(frame);
            if (st == ris::protocol::DECODE_PARTIAL) break; // 半帧：等下次数据
            if (st == ris::protocol::DECODE_BAD_TYPE) {
                LOG_WARN << "[RIS Search] 非法帧类型，断开: " << conn->name();
                conn->send(ris::protocol::encode_frame(ris::protocol::TYPE_ERROR,
                                                       "{\"error\":\"bad frame type\"}"));
                conn->shutdown();
                return;
            }
            if (st == ris::protocol::DECODE_TOO_LONG) {
                LOG_WARN << "[RIS Search] 超长帧，断开: " << conn->name();
                conn->send(ris::protocol::encode_frame(ris::protocol::TYPE_ERROR,
                                                       "{\"error\":\"frame too long\"}"));
                conn->shutdown();
                return;
            }
            // DECODE_OK
            if (frame.type == ris::protocol::TYPE_SEARCH_REQ) {
                conn->send(ris::protocol::encode_frame(ris::protocol::TYPE_SEARCH_RESP,
                                                       do_search(frame.payload)));
            } else if (frame.type == ris::protocol::TYPE_SUGGEST_REQ) {
                conn->send(ris::protocol::encode_frame(ris::protocol::TYPE_SUGGEST_RESP,
                                                       do_suggest(frame.payload)));
            }
        }
    }

    // ---------------- 检索：L1 → L2 → 索引 ----------------
    std::string do_search(const std::string &query) {
        muduo::Timestamp t0 = muduo::Timestamp::now();
        if (query.empty()) {
            return "{\"error\":\"empty query\"}";
        }
        std::shared_ptr<const ris::index::IndexSnapshot> snap = ris::index::IndexHolder::load();
        const std::string key = ris::cache::RedisCache::make_key(snap->version, query);

        // 一级缓存（进程内）
        std::string result;
        if (lru_.get(key, result)) {
            LOG_INFO << "[RIS Search] L1 命中: " << query;
            return result;
        }
        // 二级缓存（Redis，跨进程）
        if (redis_.get(key, result)) {
            lru_.put(key, result);
            LOG_INFO << "[RIS Search] L2 命中(Redis): " << query;
            return result;
        }

        // 索引检索
        std::vector<ris::index::SearchHit> hits = searcher_.search(query, 10);
        std::string j = "{\"query\":\"" + json_escape(query) + "\",\"version\":" +
                        std::to_string(snap->version) + ",\"count\":" +
                        std::to_string(hits.size()) + ",\"hits\":[";
        for (size_t i = 0; i < hits.size(); ++i) {
            ris::index::SearchHit &h = hits[i];
            bool has_image = (!h.study_instance_uid.empty() && link_.exists(h.study_instance_uid));
            char item[1024];
            std::snprintf(item, sizeof(item),
                          "%s{\"report_id\":\"%s\",\"patient\":\"%s(%s)\",\"date\":\"%s\","
                          "\"modality\":\"%s\",\"score\":%.4f,\"has_image\":%s,"
                          "\"snippet\":\"%s\"}",
                          i == 0 ? "" : ",", json_escape(h.report_id).c_str(),
                          json_escape(h.patient_name).c_str(), h.patient_id.c_str(),
                          h.exam_date.c_str(), h.modality.c_str(), h.score,
                          has_image ? "true" : "false", json_escape(h.snippet).c_str());
            j += item;
        }
        j += "]}";

        // 回填两级缓存
        lru_.put(key, j);
        redis_.setex(key, cfg_.cache_ttl, j);

        double ms = muduo::timeDifference(muduo::Timestamp::now(), t0) * 1000.0;
        LOG_INFO << "[RIS Search] 检索完成: \"" << query << "\" 命中 " << hits.size()
                 << " 条，耗时 " << ms << "ms（未命中缓存，走索引）";
        return j;
    }

    // ---------------- 纠错建议 ----------------
    std::string do_suggest(const std::string &query) {
        std::vector<ris::spell::SuggestItem> items = bk_->suggest(query, 2, 5);
        std::string j = "{\"query\":\"" + json_escape(query) + "\",\"suggestions\":[";
        for (size_t i = 0; i < items.size(); ++i) {
            char item[256];
            std::snprintf(item, sizeof(item), "%s{\"term\":\"%s\",\"distance\":%d,\"freq\":%u}",
                          i == 0 ? "" : ",", json_escape(items[i].term).c_str(),
                          items[i].distance, items[i].freq);
            j += item;
        }
        j += "]}";
        return j;
    }

    static std::string json_escape(const std::string &s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if ((unsigned char)c < 0x20) out += ' ';
            else out += c;
        }
        return out;
    }

public:
    // 服务启动后由 main 调用：加载索引 + 构建 BK-tree
    void load_components() {
        std::shared_ptr<const ris::index::IndexSnapshot> snap =
            ris::index::load_latest(cfg_.index_dir);
        ris::index::IndexHolder::store(snap);
        LOG_INFO << "[RIS Search] 索引加载: v" << snap->version << " 文档 "
                 << snap->docs.size() << " 词项 " << snap->inverted.size();
        // BK-tree 从索引词项构建（含文档频率，同距离时优先高频词）
        bk_.reset(new ris::spell::BkTree());
        for (std::unordered_map<std::string, uint32_t>::const_iterator it =
                 snap->term_df.begin();
             it != snap->term_df.end(); ++it) {
            bk_->insert(it->first, it->second);
        }
        LOG_INFO << "[RIS Search] BK-tree 就绪: " << bk_->size() << " 个词条";
    }

private:
    ris::common::RisConfig cfg_;
    muduo::net::TcpServer server_;
    std::shared_ptr<ris::index::Tokenizer> tok_;
    ris::index::Searcher searcher_;
    ris::cache::LruCache lru_;
    ris::cache::RedisCache redis_;
    StudyLink link_;
    std::shared_ptr<ris::spell::BkTree> bk_;
    std::map<std::string, std::shared_ptr<ris::protocol::TlvDecoder>> decoders_;
    std::mutex decoders_mtx_;
};

int main(int argc, char *argv[]) {
    muduo::Logger::setLogLevel(muduo::Logger::INFO);
    ris::common::RisConfig cfg = ris::common::RisConfig::from_env();

    // ---- 离线模式：构建索引 ----
    if (argc == 3 && ::strcmp(argv[1], "--build-index") == 0) {
        ris::index::Tokenizer tok(cfg.dict_dir);
        std::string err;
        int v = ris::index::build_index(argv[2], cfg.index_dir, tok, err);
        if (v < 0) {
            std::fprintf(stderr, "[RIS Search] 构建失败: %s\n", err.c_str());
            return 1;
        }
        std::printf("索引构建成功: %s/v%d\n", cfg.index_dir.c_str(), v);
        return 0;
    }

    // ---- 在线模式 ----
    LOG_INFO << "==========================================";
    LOG_INFO << " [医学影像检查报告检索系统] 启动中...";
    LOG_INFO << " - 网络核心: muduo (epoll + EventLoop 线程池, TLV 协议)";
    LOG_INFO << " - 索引目录: " << cfg.index_dir << " (dict=" << cfg.dict_dir << ")";
    LOG_INFO << " - Redis: " << cfg.redis_host << ":" << cfg.redis_port
             << " LRU容量=" << cfg.lru_capacity;
    LOG_INFO << "==========================================";

    muduo::net::EventLoop loop;
    muduo::net::InetAddress listenAddr(static_cast<uint16_t>(cfg.listen_port));
    RISSearchServer server(&loop, listenAddr, cfg);
    server.load_components();
    server.start();
    LOG_INFO << "[RIS Search] muduo TCP 检索服务已在端口 " << cfg.listen_port
             << " 监听 (" << cfg.io_threads << " 线程 Reactor)...";

    loop.loop();
    return 0;
}
