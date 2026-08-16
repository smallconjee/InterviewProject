#include "util/Sha256.h"

#include <cstdio>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "common/Logger.h"

namespace pacs {
namespace util {

Sha256::Sha256() : ctx_(NULL), ok_(false), finalized_(false) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        LOG_ERROR << "EVP_MD_CTX_new 失败";
        return;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        LOG_ERROR << "EVP_DigestInit_ex 失败";
        EVP_MD_CTX_free(ctx);
        return;
    }
    ctx_ = ctx;
    ok_ = true;
}

Sha256::~Sha256() {
    if (ctx_ != NULL) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(ctx_));
    }
}

bool Sha256::update(const void *data, size_t len) {
    if (!ok_ || finalized_) {
        return false;
    }
    return EVP_DigestUpdate(static_cast<EVP_MD_CTX *>(ctx_), data, len) == 1;
}

std::string Sha256::final_hex() {
    if (!ok_ || finalized_) {
        return "";
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (EVP_DigestFinal_ex(static_cast<EVP_MD_CTX *>(ctx_), md, &md_len) != 1) {
        return "";
    }
    finalized_ = true;
    char hex[65];
    for (unsigned int i = 0; i < md_len; ++i) {
        // 每个字节展开为两个十六进制字符
        std::snprintf(hex + i * 2, 3, "%02x", md[i]);
    }
    hex[md_len * 2] = '\0';
    return std::string(hex);
}

FileHash sha256_file(const char *path) {
    FileHash r;
    r.ok = false;
    r.size = 0;

    std::FILE *f = std::fopen(path, "rb");
    if (f == NULL) {
        r.err = "无法打开文件";
        return r;
    }

    Sha256 h;
    std::vector<char> buf(1024 * 1024); // 1MB 分块：内存占用与文件大小无关
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        if (!h.update(buf.data(), n)) {
            r.err = "哈希更新失败";
            std::fclose(f);
            return r;
        }
        r.size += n;
    }
    bool read_err = std::ferror(f);
    std::fclose(f);
    if (read_err) {
        r.err = "读取文件失败";
        return r;
    }
    r.hex = h.final_hex();
    if (r.hex.empty()) {
        r.err = "哈希收尾失败";
        return r;
    }
    r.ok = true;
    return r;
}

std::string gen_uuid_v4() {
    unsigned char b[16];
    if (RAND_bytes(b, sizeof(b)) != 1) {
        // 随机源不可用极罕见：退化为时间+地址混合，保证可用性
        LOG_WARN << "RAND_bytes 失败，task_id 使用退化随机源";
        for (int i = 0; i < 16; ++i) {
            b[i] = static_cast<unsigned char>(std::rand() & 0xFF);
        }
    }
    // RFC 4122 第 6/8 位设置版本号 4 与变体位
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    char out[37];
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(out);
}

} // namespace util
} // namespace pacs
