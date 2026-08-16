#include "archive/ArchiveDao.h"

#include <cstdio>
#include <vector>

#include "common/Logger.h"
#include "util/Sha256.h"

namespace pacs {
namespace archive {

namespace {

// SQL 字符串转义：所有进入 SQL 的字符串必须经过这里（防注入 + 防中文/引号截断）
std::string esc(MYSQL *c, const std::string &s) {
    std::vector<char> buf(2 * s.size() + 1);
    unsigned long n = ::mysql_real_escape_string_quote(c, buf.data(), s.c_str(),
                                                       s.size(), '\'');
    return std::string(buf.data(), n);
}

bool exec_sql(MYSQL *c, const std::string &sql, std::string &err) {
    if (::mysql_query(c, sql.c_str()) != 0) {
        err = std::string("SQL 失败: ") + ::mysql_error(c) + " | sql=" + sql.substr(0, 200);
        return false;
    }
    return true;
}

// DICOM 日期 YYYYMMDD → SQL 'YYYY-MM-DD'；非法/空返回 NULL
std::string dicom_date_sql(const std::string &d) {
    if (d.size() != 8) return "NULL";
    for (size_t i = 0; i < 8; ++i) {
        if (d[i] < '0' || d[i] > '9') return "NULL";
    }
    return "'" + d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2) + "'";
}

// 性别：合法 M/F/O 原样，否则 NULL（表上有 CHECK 约束）
std::string sex_sql(const std::string &s) {
    if (s == "M" || s == "F" || s == "O") return "'" + s + "'";
    return "NULL";
}

} // namespace

ArchiveDao::ArchiveDao(db::MySQLPool &pool) : pool_(pool) {}

// ---------------- 层级 upsert：ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id) ----------------
// 该技巧让 mysql_insert_id 在"新插入"和"撞唯一键"两种情况下都返回行 id，
// 一条 SQL 完成 upsert + 取 id，无需先 SELECT 再判断（也消除了查插竞态）。

bool ArchiveDao::upsert_patient(MYSQL *c, const dicom::DicomInfo &info, uint64_t &id,
                                std::string &err) {
    std::string sql = "INSERT INTO patient(patient_id, issuer, patient_name, birth_date, sex) VALUES ('" +
                      esc(c, info.patient_id) + "','" + esc(c, info.issuer) + "','" +
                      esc(c, info.patient_name) + "'," + dicom_date_sql(info.birth_date) + "," +
                      sex_sql(info.sex) + ") ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id), patient_name=VALUES(patient_name)";
    if (!exec_sql(c, sql, err)) return false;
    id = ::mysql_insert_id(c);
    return true;
}

bool ArchiveDao::upsert_study(MYSQL *c, const dicom::DicomInfo &info, uint64_t patient_id,
                              uint64_t &id, std::string &err) {
    std::string sql = "INSERT INTO study(patient_fk, study_instance_uid, study_date, accession_number, study_description) VALUES (" +
                      std::to_string(patient_id) + ",'" + esc(c, info.study_instance_uid) + "'," +
                      dicom_date_sql(info.study_date) + ",'" + esc(c, info.accession_number) +
                      "','" + esc(c, info.study_description) +
                      "') ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id), study_date=VALUES(study_date)";
    if (!exec_sql(c, sql, err)) return false;
    id = ::mysql_insert_id(c);
    return true;
}

bool ArchiveDao::upsert_series(MYSQL *c, const dicom::DicomInfo &info, uint64_t study_id,
                               uint64_t &id, std::string &err) {
    std::string num = info.series_number >= 0 ? std::to_string(info.series_number) : "NULL";
    std::string sql = "INSERT INTO series(study_fk, series_instance_uid, modality, series_number) VALUES (" +
                      std::to_string(study_id) + ",'" + esc(c, info.series_instance_uid) + "','" +
                      esc(c, info.modality) + "'," + num + ") ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id)";
    if (!exec_sql(c, sql, err)) return false;
    id = ::mysql_insert_id(c);
    return true;
}

// ---------------- 事务一：幂等决策树 ----------------

BeginResult ArchiveDao::begin_archive(const dicom::DicomInfo &info, const std::string &sha256_hex,
                                      uint64_t file_size) {
    BeginResult res;
    db::MySQLGuard guard = pool_.acquire();
    if (!guard) {
        res.decision = IMPORT_DB_ERROR;
        res.err = "获取数据库连接超时";
        return res;
    }
    MYSQL *c = guard.get();
    std::string err;

    if (!exec_sql(c, "START TRANSACTION", err)) {
        res.decision = IMPORT_DB_ERROR;
        res.err = err;
        return res;
    }

    // FOR UPDATE 锁住该 UID 的行（唯一索引），并发同 UID 导入在此排队，
    // 决策树因此不会出现两个事务同时判定"不存在"
    std::string sel = "SELECT id, status, sha256 FROM sop_instance WHERE sop_instance_uid='" +
                      esc(c, info.sop_instance_uid) + "' FOR UPDATE";
    if (!exec_sql(c, sel, err)) {
        ::mysql_rollback(c);
        res.decision = IMPORT_DB_ERROR;
        res.err = err;
        return res;
    }
    MYSQL_RES *rs = ::mysql_store_result(c);
    bool exists = (rs != NULL && ::mysql_num_rows(rs) > 0);
    uint64_t exist_id = 0;
    std::string exist_status, exist_sha;
    if (exists) {
        MYSQL_ROW row = ::mysql_fetch_row(rs);
        exist_id = std::strtoull(row[0], NULL, 10);
        exist_status = row[1] ? row[1] : "";
        exist_sha = row[2] ? row[2] : "";
    }
    ::mysql_free_result(rs);

    if (exists) {
        if (exist_sha == sha256_hex) {
            // 同 UID 同哈希：
            //   ARCHIVED → 幂等重复导入，直接成功返回
            //   PARSED   → 上次中断，走续传（刷新层级引用后等事务二）
            if (exist_status == "ARCHIVED") {
                ::mysql_commit(c);
                res.decision = IMPORT_DUPLICATE;
                res.instance_id = exist_id;
                return res;
            }
            uint64_t pid = 0, sid = 0, seid = 0;
            if (!upsert_patient(c, info, pid, err) || !upsert_study(c, info, pid, sid, err) ||
                !upsert_series(c, info, sid, seid, err)) {
                ::mysql_rollback(c);
                res.decision = IMPORT_DB_ERROR;
                res.err = err;
                return res;
            }
            std::string upd = "UPDATE sop_instance SET series_fk=" + std::to_string(seid) +
                              ", file_size=" + std::to_string(file_size) +
                              " WHERE id=" + std::to_string(exist_id);
            if (!exec_sql(c, upd, err)) {
                ::mysql_rollback(c);
                res.decision = IMPORT_DB_ERROR;
                res.err = err;
                return res;
            }
            if (::mysql_commit(c) != 0) {
                res.decision = IMPORT_DB_ERROR;
                res.err = std::string("提交失败: ") + ::mysql_error(c);
                return res;
            }
            res.decision = IMPORT_RESUME;
            res.instance_id = exist_id;
            return res;
        }
        // 同 UID 异哈希：一律拒绝。
        // 已归档数据保持原样（不因一次错误上传被污染）；PARSED 行标记 CONFLICT 便于排障
        if (exist_status != "ARCHIVED") {
            std::string upd = "UPDATE sop_instance SET status='CONFLICT' WHERE id=" +
                              std::to_string(exist_id);
            if (!exec_sql(c, upd, err)) {
                ::mysql_rollback(c);
                res.decision = IMPORT_DB_ERROR;
                res.err = err;
                return res;
            }
        }
        ::mysql_commit(c);
        res.decision = IMPORT_CONFLICT;
        res.instance_id = exist_id;
        return res;
    }

    // 新对象：层级 upsert + instance(PARSED)
    uint64_t pid = 0, sid = 0, seid = 0;
    if (!upsert_patient(c, info, pid, err) || !upsert_study(c, info, pid, sid, err) ||
        !upsert_series(c, info, sid, seid, err)) {
        ::mysql_rollback(c);
        res.decision = IMPORT_DB_ERROR;
        res.err = err;
        return res;
    }
    std::string inum = info.instance_number >= 0 ? std::to_string(info.instance_number) : "NULL";
    std::string ins = "INSERT INTO sop_instance(series_fk, sop_instance_uid, sop_class_uid, instance_number, sha256, file_size, status) VALUES (" +
                      std::to_string(seid) + ",'" + esc(c, info.sop_instance_uid) + "','" +
                      esc(c, info.sop_class_uid) + "'," + inum + ",'" + esc(c, sha256_hex) +
                      "'," + std::to_string(file_size) + ",'PARSED')";
    if (!exec_sql(c, ins, err)) {
        ::mysql_rollback(c);
        res.decision = IMPORT_DB_ERROR;
        res.err = err;
        return res;
    }
    // mysql_insert_id 必须在 COMMIT 之前取：COMMIT 之后的"最后语句"不再是 INSERT，
    // 会返回 0（本 bug 现场：instance_fk=0 触发外键约束失败，实例卡在 PARSED）
    uint64_t new_instance_id = ::mysql_insert_id(c);
    if (::mysql_commit(c) != 0) {
        res.decision = IMPORT_DB_ERROR;
        res.err = std::string("提交失败: ") + ::mysql_error(c);
        return res;
    }
    res.decision = IMPORT_NEW;
    res.instance_id = new_instance_id;
    return res;
}

// ---------------- 事务二：ARCHIVED + 本地消息表（同事务） ----------------

bool ArchiveDao::finish_archive(uint64_t instance_id, const std::string &storage_path,
                                std::string &task_id, std::string &err) {
    db::MySQLGuard guard = pool_.acquire();
    if (!guard) {
        err = "获取数据库连接超时";
        return false;
    }
    MYSQL *c = guard.get();

    if (!exec_sql(c, "START TRANSACTION", err)) return false;

    std::string task = util::gen_uuid_v4();
    std::string upd = "UPDATE sop_instance SET status='ARCHIVED', storage_path='" +
                      esc(c, storage_path) + "', backup_status='PENDING' WHERE id=" +
                      std::to_string(instance_id);
    if (!exec_sql(c, upd, err)) {
        ::mysql_rollback(c);
        return false;
    }

    // ★ 本地消息表插入与归档状态变更同一事务：要么都可见、要么都不存在。
    // 这是异步链"消息必不丢"的第一道防线；第二道是消费端补偿扫描（阶段 2）
    std::string evt = "INSERT INTO backup_event(task_id, instance_fk, status) VALUES ('" +
                      esc(c, task) + "'," + std::to_string(instance_id) + ",'PENDING')";
    if (!exec_sql(c, evt, err)) {
        ::mysql_rollback(c);
        return false;
    }

    if (::mysql_commit(c) != 0) {
        err = std::string("提交失败: ") + ::mysql_error(c);
        ::mysql_rollback(c);
        return false;
    }
    task_id = task;
    return true;
}

// ---------------- 查询 ----------------

bool ArchiveDao::get_instance(const std::string &sop_uid, InstanceRow &row, bool &found,
                              std::string &err) {
    found = false;
    db::MySQLGuard guard = pool_.acquire();
    if (!guard) {
        err = "获取数据库连接超时";
        return false;
    }
    MYSQL *c = guard.get();
    std::string sql = "SELECT id, sop_instance_uid, status, backup_status, sha256, file_size, storage_path FROM sop_instance WHERE sop_instance_uid='" +
                      esc(c, sop_uid) + "'";
    if (!exec_sql(c, sql, err)) return false;
    MYSQL_RES *rs = ::mysql_store_result(c);
    if (rs == NULL) {
        err = ::mysql_error(c);
        return false;
    }
    if (::mysql_num_rows(rs) == 0) {
        ::mysql_free_result(rs);
        return true; // 查无此行不是错误
    }
    MYSQL_ROW r = ::mysql_fetch_row(rs);
    row.id = std::strtoull(r[0], NULL, 10);
    row.sop_instance_uid = r[1] ? r[1] : "";
    row.status = r[2] ? r[2] : "";
    row.backup_status = r[3] ? r[3] : "";
    row.sha256 = r[4] ? r[4] : "";
    row.file_size = r[5] ? std::strtoull(r[5], NULL, 10) : 0;
    row.storage_path = r[6] ? r[6] : "";
    ::mysql_free_result(rs);
    found = true;
    return true;
}

bool ArchiveDao::list_studies(const std::string &patient_id, const std::string &issuer,
                              std::vector<StudyRow> &out, std::string &err) {
    db::MySQLGuard guard = pool_.acquire();
    if (!guard) {
        err = "获取数据库连接超时";
        return false;
    }
    MYSQL *c = guard.get();
    std::string sql =
        "SELECT s.id, s.study_instance_uid, p.patient_name, p.patient_id, p.issuer, "
        "IFNULL(DATE_FORMAT(s.study_date,'%Y-%m-%d'),''), s.accession_number, s.study_description, "
        "(SELECT COUNT(*) FROM sop_instance i JOIN series se ON i.series_fk=se.id WHERE se.study_fk=s.id) "
        "FROM study s JOIN patient p ON s.patient_fk=p.id";
    if (!patient_id.empty()) {
        sql += " WHERE p.patient_id='" + esc(c, patient_id) + "'";
        if (!issuer.empty()) {
            sql += " AND p.issuer='" + esc(c, issuer) + "'";
        }
    }
    sql += " ORDER BY s.study_date DESC, s.id DESC LIMIT 200";
    if (!exec_sql(c, sql, err)) return false;
    MYSQL_RES *rs = ::mysql_store_result(c);
    if (rs == NULL) {
        err = ::mysql_error(c);
        return false;
    }
    MYSQL_ROW r;
    while ((r = ::mysql_fetch_row(rs)) != NULL) {
        StudyRow row;
        row.study_id = std::strtoull(r[0], NULL, 10);
        row.study_instance_uid = r[1] ? r[1] : "";
        row.patient_name = r[2] ? r[2] : "";
        row.patient_id = r[3] ? r[3] : "";
        row.issuer = r[4] ? r[4] : "";
        row.study_date = r[5] ? r[5] : "";
        row.accession_number = r[6] ? r[6] : "";
        row.study_description = r[7] ? r[7] : "";
        row.instance_count = r[8] ? std::atoi(r[8]) : 0;
        out.push_back(row);
    }
    ::mysql_free_result(rs);
    return true;
}

bool ArchiveDao::get_series_of_study(const std::string &study_uid, std::vector<SeriesRow> &out,
                                     std::string &err) {
    db::MySQLGuard guard = pool_.acquire();
    if (!guard) {
        err = "获取数据库连接超时";
        return false;
    }
    MYSQL *c = guard.get();
    std::string sql =
        "SELECT se.id, se.series_instance_uid, se.modality, IFNULL(se.series_number,-1), "
        "(SELECT COUNT(*) FROM sop_instance i WHERE i.series_fk=se.id) "
        "FROM series se JOIN study s ON se.study_fk=s.id WHERE s.study_instance_uid='" +
        esc(c, study_uid) + "' ORDER BY se.series_number";
    if (!exec_sql(c, sql, err)) return false;
    MYSQL_RES *rs = ::mysql_store_result(c);
    if (rs == NULL) {
        err = ::mysql_error(c);
        return false;
    }
    MYSQL_ROW r;
    while ((r = ::mysql_fetch_row(rs)) != NULL) {
        SeriesRow row;
        row.series_id = std::strtoull(r[0], NULL, 10);
        row.series_instance_uid = r[1] ? r[1] : "";
        row.modality = r[2] ? r[2] : "";
        row.series_number = r[3] ? std::atoi(r[3]) : -1;
        row.instance_count = r[4] ? std::atoi(r[4]) : 0;
        out.push_back(row);
    }
    ::mysql_free_result(rs);
    return true;
}

bool ArchiveDao::list_recent_backup_events(int limit, std::vector<BackupEventRow> &out,
                                           std::string &err) {
    db::MySQLGuard guard = pool_.acquire();
    if (!guard) {
        err = "获取数据库连接超时";
        return false;
    }
    MYSQL *c = guard.get();
    std::string sql = "SELECT task_id, instance_fk, status, retry_count FROM backup_event "
                      "ORDER BY id DESC LIMIT " + std::to_string(limit);
    if (!exec_sql(c, sql, err)) return false;
    MYSQL_RES *rs = ::mysql_store_result(c);
    if (rs == NULL) {
        err = ::mysql_error(c);
        return false;
    }
    MYSQL_ROW r;
    while ((r = ::mysql_fetch_row(rs)) != NULL) {
        BackupEventRow row;
        row.task_id = r[0] ? r[0] : "";
        row.instance_fk = r[1] ? std::strtoull(r[1], NULL, 10) : 0;
        row.status = r[2] ? r[2] : "";
        row.retry_count = r[3] ? std::atoi(r[3]) : 0;
        out.push_back(row);
    }
    ::mysql_free_result(rs);
    return true;
}

} // namespace archive
} // namespace pacs
