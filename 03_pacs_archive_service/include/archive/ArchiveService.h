// ============================================================================
// ArchiveService.h — 导入主链路编排（SeriesWork 的逻辑等价物，逐步同步实现）
//
// 落盘 → 解析 → 流式哈希 → 事务一(决策) → rename 到最终路径 → 事务二(归档+消息表)
// 崩溃窗口分析（面试讲点）：
//   - 写 staging 中崩溃：无 DB 行，重传从头开始（staging 用 UUID 文件名，互不干扰）
//   - 事务一后、rename 前崩溃：实例停在 PARSED，重传走 RESUME 分支
//   - rename 后、事务二前崩溃：PARSED + 最终文件已在位，重传覆盖同名文件后补齐事务二
// staging 与最终对象同在 storage_dir 下：rename(2) 原子且不跨文件系统
// ============================================================================
#pragma once

#include <string>

#include "archive/ArchiveDao.h"
#include "common/Config.h"

namespace pacs {
namespace archive {

// 导入接口的最终结果（HTTP 层直接映射状态码）
struct ImportResult {
    int http_status;        // 201 新建 / 200 幂等重复 / 409 冲突 / 500 失败
    std::string status_word; // ARCHIVED / DUPLICATE / CONFLICT / FAILED
    uint64_t instance_id;
    std::string task_id;    // 备份任务（DUPLICATE 时为空）
    std::string detail;     // 附加信息（错误原因等）
};

class ArchiveService {
public:
    ArchiveService(const common::Config &cfg, db::MySQLPool &pool);

    // body 为一个完整 DICOM 文件的内容（octet-stream 或 multipart 文件域）
    ImportResult import_image(const char *data, size_t size);

private:
    // 确保 storage_dir/staging 存在；返回最终对象相对路径（入库的 storage_path）
    bool ensure_dirs(std::string &err);
    std::string final_rel_path(const dicom::DicomInfo &info) const;

    common::Config cfg_;
    ArchiveDao dao_;
};

} // namespace archive
} // namespace pacs
