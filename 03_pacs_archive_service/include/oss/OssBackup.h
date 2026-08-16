// ============================================================================
// OssBackup.h — 阿里云 OSS 异步备份封装
//
// 凭据只从环境变量注入（PACS_OSS_ENDPOINT/AK/SK/BUCKET），绝不进代码与配置文件。
// 上传 key 复用归档相对路径（studyUID/seriesUID/sopUID.dcm），与本地存储布局
// 一致，恢复时可直接按路径回拷。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace pacs {
namespace oss {

class OssBackup {
public:
    OssBackup();
    ~OssBackup();

    // 读环境变量初始化客户端；任一变量缺失返回 false（阶段 2 前允许不配置）
    bool init(std::string &err);

    // 把本地文件上传为 bucket/<key>；err 返回失败原因（驱动消费端有界重试）
    bool put_object(const std::string &local_path, const std::string &key, std::string &err);

    bool ready() const { return client_ != NULL; }

private:
    void *client_;   // AlibabaCloud::OSS::OssClient*（头文件不暴露 SDK 头）
    std::string bucket_;
};

} // namespace oss
} // namespace pacs
