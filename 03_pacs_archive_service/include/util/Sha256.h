// ============================================================================
// Sha256.h — SHA-256 流式哈希（OpenSSL EVP + RAII）
//
// 为什么流式而不是整个读进内存：DICOM 实例可达几百 MB，EVP_DigestUpdate
// 分块喂入使内存占用恒定（1MB 缓冲），这正是简历 bullet 3 "分块读取流式计算"
// 的实现。RAII：EVP_MD_CTX 在析构中释放，异常路径不泄漏 OpenSSL 上下文。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace pacs {
namespace util {

class Sha256 {
public:
    Sha256();
    ~Sha256();

    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;

    // 追加一段数据；初始化失败后调用返回 false
    bool update(const void *data, size_t len);

    // 结束并输出 64 字符小写十六进制；只能调用一次
    std::string final_hex();

private:
    void *ctx_;      // 实际类型 EVP_MD_CTX*，头文件不暴露 OpenSSL 头
    bool ok_;
    bool finalized_;
};

// 文件级便捷接口：按 1MB 分块读取并流式计算（body 是文件而不是内存块时用）
struct FileHash {
    bool ok;
    std::string hex;     // 64 字符小写十六进制
    uint64_t size;       // 顺便返回文件大小（入库要用）
    std::string err;
};
FileHash sha256_file(const char *path);

// 生成 36 字符 UUID v4（backup_event.task_id 用；OpenSSL 随机源）
std::string gen_uuid_v4();

} // namespace util
} // namespace pacs
