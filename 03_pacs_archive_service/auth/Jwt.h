// ============================================================================
// Jwt.h — 手写 JWT HS256（签发 + 校验）
//
// 为什么手写而不用 jwt-cpp：JWT 本质是 base64url(header).base64url(payload).
// HMAC-SHA256(signature) 三段拼接，~150 行可控可讲；OpenSSL 提供 HMAC 与
// 时间安全的 CRYPTO_memcmp，不引入额外依赖。面试时能手画这个结构就是加分项。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace pacs {
namespace auth {

struct JwtClaims {
    std::string username; // sub
    std::string role;
    int64_t expires_at;   // unix 秒
};

// 签发：payload = {"sub":..,"role":..,"iat":now,"exp":now+ttl}
std::string jwt_sign(const std::string &username, const std::string &role,
                     int64_t ttl_seconds, const std::string &secret);

// 校验：签名(CRYPTO_memcmp 防时序比较攻击) + 过期时间
bool jwt_verify(const std::string &token, const std::string &secret, JwtClaims &out,
                std::string &err);

} // namespace auth
} // namespace pacs
