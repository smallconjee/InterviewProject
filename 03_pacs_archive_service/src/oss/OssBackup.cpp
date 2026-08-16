#include "oss/OssBackup.h"

#include <cstdlib>
#include <fstream>

#include <alibabacloud/oss/OssClient.h>

#include "common/Logger.h"

namespace pacs {
namespace oss {

OssBackup::OssBackup() : client_(NULL) {}

OssBackup::~OssBackup() {
    if (client_ != NULL) {
        delete reinterpret_cast<AlibabaCloud::OSS::OssClient *>(client_);
    }
}

bool OssBackup::init(std::string &err) {
    const char *endpoint = ::getenv("PACS_OSS_ENDPOINT");
    const char *ak = ::getenv("PACS_OSS_AK");
    const char *sk = ::getenv("PACS_OSS_SK");
    const char *bucket = ::getenv("PACS_OSS_BUCKET");
    if (endpoint == NULL || ak == NULL || sk == NULL || bucket == NULL ||
        *endpoint == '\0' || *ak == '\0' || *sk == '\0' || *bucket == '\0') {
        err = "OSS 未配置：需要环境变量 PACS_OSS_ENDPOINT / PACS_OSS_AK / PACS_OSS_SK / PACS_OSS_BUCKET";
        return false;
    }
    bucket_ = bucket;

    AlibabaCloud::OSS::ClientConfiguration conf;
    // 连接/请求超时：备份是异步链路，宁可失败重试也不要长时间挂住消费线程
    conf.requestTimeoutMs = 30000;
    client_ = new AlibabaCloud::OSS::OssClient(endpoint, ak, sk, conf);
    LOG_INFO << "OSS 客户端就绪: bucket=" << bucket_ << " endpoint=" << endpoint;
    return true;
}

bool OssBackup::put_object(const std::string &local_path, const std::string &key,
                           std::string &err) {
    if (client_ == NULL) {
        err = "OSS 客户端未初始化";
        return false;
    }
    // PutObject 接受 iostream：用 fstream(in|binary) 打开（ifstream 不是 iostream）
    std::shared_ptr<std::iostream> content = std::make_shared<std::fstream>(
        local_path.c_str(), std::ios::in | std::ios::binary);
    if (!*content) {
        err = "打开本地文件失败: " + local_path;
        return false;
    }

    AlibabaCloud::OSS::OssClient *client =
        reinterpret_cast<AlibabaCloud::OSS::OssClient *>(client_);
    auto outcome = client->PutObject(bucket_, key, content);
    if (!outcome.isSuccess()) {
        err = "PutObject 失败: " + outcome.error().Message() +
              " (code=" + outcome.error().Code() + ")";
        return false;
    }
    LOG_INFO << "[OSS] 上传成功: " << key << " <- " << local_path;
    return true;
}

} // namespace oss
} // namespace pacs
