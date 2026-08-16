// ============================================================================
// RedisCache.h — 二级缓存（跨进程共享，hiredis 同步客户端）
//
// 缓存键设计（简历 RIS bullet 5 的核心考点）：
//   key = ris:v{索引版本号}:{FNV-1a64(查询词)} 
//   索引重建后版本号变化 → 新查询天然落在新键上，旧版本键靠 TTL 自然过期，
//   不需要主动清理——"将索引版本号纳入缓存键"的原文实现。
// FNV-1a：跨进程/跨版本稳定的 64 位哈希（std::hash 的种子实现相关，不可作共享键）
// 降级策略：Redis 不可用时缓存层静默失败（记一次 WARN），检索照常走索引——
//   缓存是加速器不是依赖。
// ============================================================================
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace ris {
namespace cache {

class RedisCache {
public:
    RedisCache(const std::string &host, int port);

    ~RedisCache();

    // get/setex 均带互斥保护：hiredis 同步上下文非线程安全
    bool get(const std::string &key, std::string &out);
    bool setex(const std::string &key, int ttl_seconds, const std::string &value);

    // 稳定哈希：FNV-1a 64 位，输出 16 字符十六进制
    static std::string hash_key(const std::string &query);

    // 组装版本化缓存键
    static std::string make_key(int index_version, const std::string &query);

private:
    // 调用方必须持有 mtx_（get/setex 内部调用；不能自持锁否则死锁）
    bool ensure_connected();

    std::string host_;
    int port_;
    void *ctx_; // redisContext*（头文件不暴露 hiredis）
    bool warned_; // Redis 故障只告警一次，避免刷屏
    // hiredis 同步上下文非线程安全，而本对象被 4 个 muduo IO 线程并发调用
    // （每条连接的 onMessage → do_search → redis_.get/setex）——必须整体互斥。
    // 已知取舍：临界区含一次网络往返（~1ms 量级），4 线程的缓存查询会串行化；
    // 当前量级（长连接低频查询）可接受。演进点：每 IO 线程独立连接或小连接池，
    // 消除串行化——与 LruCache 的细粒度锁形成对比讲点。
    mutable std::mutex mtx_;
};

} // namespace cache
} // namespace ris
