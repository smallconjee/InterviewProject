#include "cache/RedisCache.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include <hiredis/hiredis.h>

#include <muduo/base/Logging.h>

namespace ris {
namespace cache {

namespace {

// FNV-1a 64 位：分布均匀且跨实现稳定，适合做共享缓存键
uint64_t fnv1a64(const std::string &s) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < s.size(); ++i) {
        h ^= static_cast<unsigned char>(s[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace

RedisCache::RedisCache(const std::string &host, int port)
    : host_(host), port_(port), ctx_(NULL), warned_(false) {}

RedisCache::~RedisCache() {
    if (ctx_ != NULL) {
        ::redisFree(reinterpret_cast<redisContext *>(ctx_));
    }
}

bool RedisCache::ensure_connected() {
    if (ctx_ != NULL) {
        return true;
    }
    struct timeval tv = {2, 0}; // 连接/读写超时 2s：缓存层不允许拖垮检索
    redisContext *ctx = ::redisConnectWithTimeout(host_.c_str(), port_, tv);
    if (ctx == NULL || ctx->err != 0) {
        if (ctx != NULL) ::redisFree(ctx);
        if (!warned_) {
            LOG_WARN << "[RIS Search] Redis 不可用(" << host_ << ":" << port_
                     << ")，二级缓存降级为直查索引";
            warned_ = true;
        }
        return false;
    }
    ctx_ = ctx;
    return true;
}

bool RedisCache::get(const std::string &key, std::string &out) {
    if (!ensure_connected()) return false;
    redisContext *ctx = reinterpret_cast<redisContext *>(ctx_);
    redisReply *reply = static_cast<redisReply *>(::redisCommand(ctx, "GET %s", key.c_str()));
    if (reply == NULL) {
        // 连接断开：清理上下文，下次重连
        ::redisFree(ctx);
        ctx_ = NULL;
        return false;
    }
    bool hit = (reply->type == REDIS_REPLY_STRING && reply->str != NULL);
    if (hit) {
        out.assign(reply->str, reply->len);
    }
    ::freeReplyObject(reply);
    return hit;
}

bool RedisCache::setex(const std::string &key, int ttl_seconds, const std::string &value) {
    if (!ensure_connected()) return false;
    redisContext *ctx = reinterpret_cast<redisContext *>(ctx_);
    redisReply *reply = static_cast<redisReply *>(
        ::redisCommand(ctx, "SETEX %s %d %b", key.c_str(), ttl_seconds, value.data(),
                       value.size()));
    if (reply == NULL) {
        ::redisFree(ctx);
        ctx_ = NULL;
        return false;
    }
    bool ok = (reply->type != REDIS_REPLY_ERROR);
    ::freeReplyObject(reply);
    return ok;
}

std::string RedisCache::hash_key(const std::string &query) {
    char buf[17];
    uint64_t h = fnv1a64(query);
    ::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::string RedisCache::make_key(int index_version, const std::string &query) {
    // 版本号进键：索引切换后旧键永不命中，靠 TTL(300s) 自然回收
    return "ris:v" + std::to_string(index_version) + ":" + hash_key(query);
}

} // namespace cache
} // namespace ris
