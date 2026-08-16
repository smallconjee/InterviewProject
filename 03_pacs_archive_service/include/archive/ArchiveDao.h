// ============================================================================
// ArchiveDao.h — 归档数据访问层：幂等决策树 + 四层 upsert + 本地消息表
//
// 事务设计（与简历 bullet 3/4 对应）：
//   事务一 begin_archive：SELECT ... FOR UPDATE 锁实例行 → 决策
//     - 不存在 → upsert patient/study/series + INSERT instance(PARSED) → 新对象
//     - PARSED（上次中断）→ 刷新层级/哈希 → 续传
//     - ARCHIVED 且哈希一致 → DUPLICATE（幂等返回，不改数据）
//     - ARCHIVED/PARSED 且哈希不同 → 拒绝（CONFICT，不污染已归档数据）
//   事务二 finish_archive：UPDATE instance → ARCHIVED + storage_path，
//     并【同一事务】INSERT backup_event(PENDING) ★本地消息表与归档状态同事务提交★
// 两事务之间的崩溃窗口由 PARSED 状态 + staging 文件兜底（重传即续传）。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/Config.h"
#include "db/MySQLPool.h"
#include "dicom/DicomReader.h"

namespace pacs {
namespace archive {

// 幂等决策树的结果（导入接口据此返回 201/200/409）
enum ImportDecision {
    IMPORT_NEW = 0,       // 新实例，事务一完成，等待事务二
    IMPORT_RESUME,        // 中断后续传，事务一完成，等待事务二
    IMPORT_DUPLICATE,     // 同 UID 同哈希：幂等成功，数据未变
    IMPORT_CONFLICT,      // 同 UID 异哈希：拒绝，数据未变
    IMPORT_DB_ERROR,      // 数据库错误
};

struct BeginResult {
    ImportDecision decision;
    uint64_t instance_id = 0;
    std::string err;
};

// 实例行（状态查询接口用）
struct InstanceRow {
    uint64_t id;
    std::string sop_instance_uid;
    std::string status;
    std::string backup_status;
    std::string sha256;
    uint64_t file_size;
    std::string storage_path;
};

// 查询结果行
struct StudyRow {
    uint64_t study_id;
    std::string study_instance_uid;
    std::string patient_name;
    std::string patient_id;
    std::string issuer;
    std::string study_date;
    std::string accession_number;
    std::string study_description;
    int instance_count;
};

struct SeriesRow {
    uint64_t series_id;
    std::string series_instance_uid;
    std::string modality;
    int series_number;
    int instance_count;
};

class ArchiveDao {
public:
    ArchiveDao(db::MySQLPool &pool);

    // 事务一：决策 + 层级 upsert + instance(PARSED)。DUPLICATE/CONFLICT 不改数据。
    BeginResult begin_archive(const dicom::DicomInfo &info, const std::string &sha256_hex,
                              uint64_t file_size);

    // 事务二：置 ARCHIVED + storage_path，同事务写 backup_event(PENDING)。
    // 成功时 task_id 返回给调用方（也是消费端幂等键）。
    bool finish_archive(uint64_t instance_id, const std::string &storage_path,
                        std::string &task_id, std::string &err);

    // 单实例状态查询（幂等结果核对/排障用）
    bool get_instance(const std::string &sop_uid, InstanceRow &row, bool &found, std::string &err);

    // 列表查询
    bool list_studies(const std::string &patient_id, const std::string &issuer,
                      std::vector<StudyRow> &out, std::string &err);
    bool get_series_of_study(const std::string &study_uid, std::vector<SeriesRow> &out,
                             std::string &err);

    // 备份事件流水（管理员观测接口用：本地消息表状态机一目了然）
    struct BackupEventRow {
        std::string task_id;
        uint64_t instance_fk;
        std::string status;
        int retry_count;
    };
    bool list_recent_backup_events(int limit, std::vector<BackupEventRow> &out,
                                   std::string &err);

private:
    // 在【当前连接】上按 UID upsert 一层，返回行 id（靠 LAST_INSERT_ID(id) 技巧）
    bool upsert_patient(MYSQL *c, const dicom::DicomInfo &info, uint64_t &id, std::string &err);
    bool upsert_study(MYSQL *c, const dicom::DicomInfo &info, uint64_t patient_id,
                      uint64_t &id, std::string &err);
    bool upsert_series(MYSQL *c, const dicom::DicomInfo &info, uint64_t study_id,
                       uint64_t &id, std::string &err);

    db::MySQLPool &pool_;
};

} // namespace archive
} // namespace pacs
