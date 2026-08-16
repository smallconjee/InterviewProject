#include "Jwt.h"

#include <ctime>
#include <cstring>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <nlohmann/json.hpp>

namespace pacs {
namespace auth {

namespace {

// baseurl 编码表（RFC 4648 §5：'+'→'-'，'/'→'_'，去掉 '=' 填充）
std::string b64url_encode(const unsigned char *data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i + 1 == len) { // 余 1 字节 → 2 字符
        uint32_t v = data[i] << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
    } else if (i + 2 == len) { // 余 2 字节 → 3 字符
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
    }
    return out;
}

int b64url_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

bool b64url_decode(const std::string &in, std::vector<unsigned char> &out) {
    out.clear();
    out.reserve(in.size() / 4 * 3 + 3);
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        int v = b64url_decode_char(in[i]);
        if (v < 0) return false;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

std::string hmac_sha256(const std::string &key, const std::string &data) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    ::HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char *>(data.data()), data.size(), mac, &mac_len);
    return b64url_encode(mac, mac_len);
}

} // namespace

std::string jwt_sign(const std::string &username, const std::string &role,
                     int64_t ttl_seconds, const std::string &secret) {
    int64_t now = static_cast<int64_t>(::time(NULL));
    nlohmann::json payload;
    payload["sub"] = username;
    payload["role"] = role;
    payload["iat"] = now;
    payload["exp"] = now + ttl_seconds;

    // 头部固定 HS256；JSON 序列化在签发侧是确定性的（同一库版本）
    std::string header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    std::string head_b = b64url_encode(reinterpret_cast<const unsigned char *>(header.data()),
                                       header.size());
    std::string body = payload.dump();
    std::string body_b = b64url_encode(reinterpret_cast<const unsigned char *>(body.data()),
                                       body.size());
    std::string signing = head_b + "." + body_b;
    return signing + "." + hmac_sha256(secret, signing);
}

bool jwt_verify(const std::string &token, const std::string &secret, JwtClaims &out,
                std::string &err) {
    // 三段拆分
    size_t d1 = token.find('.');
    size_t d2 = token.rfind('.');
    if (d1 == std::string::npos || d2 == std::string::npos || d1 == d2) {
        err = "token 结构非法";
        return false;
    }
    std::string head_b = token.substr(0, d1);
    std::string body_b = token.substr(d1 + 1, d2 - d1 - 1);
    std::string sig_b = token.substr(d2 + 1);

    // 签名校验：CRYPTO_memcmp 常数时间比较，防时序侧信道逐字节猜测
    std::string expect = hmac_sha256(secret, head_b + "." + body_b);
    if (expect.size() != sig_b.size() ||
        ::CRYPTO_memcmp(expect.data(), sig_b.data(), expect.size()) != 0) {
        err = "签名不匹配";
        return false;
    }

    // payload 解析与过期校验
    std::vector<unsigned char> body_raw;
    if (!b64url_decode(body_b, body_raw)) {
        err = "payload base64 解码失败";
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(body_raw.begin(), body_raw.end());
        out.username = j.value("sub", "");
        out.role = j.value("role", "");
        out.expires_at = j.value("exp", static_cast<int64_t>(0));
    } catch (const std::exception &e) {
        err = std::string("payload 解析失败: ") + e.what();
        return false;
    }
    if (out.username.empty()) {
        err = "payload 缺少 sub";
        return false;
    }
    if (out.expires_at <= static_cast<int64_t>(::time(NULL))) {
        err = "token 已过期";
        return false;
    }
    return true;
}

} // namespace auth
} // namespace pacs
