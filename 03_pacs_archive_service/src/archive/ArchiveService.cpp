#include "archive/ArchiveService.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "common/Logger.h"
#include "util/Sha256.h"

namespace pacs {
namespace archive {

namespace {

// mkdir -p 等价物（C++11 无 std::filesystem，用 POSIX mkdir 逐级创建）
bool mkdir_p(const std::string &dir) {
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] == '/' || i + 1 == dir.size()) {
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

ArchiveService::ArchiveService(const common::Config &cfg, db::MySQLPool &pool)
    : cfg_(cfg), dao_(pool) {}

bool ArchiveService::ensure_dirs(std::string &err) {
    if (!mkdir_p(cfg_.server.storage_dir + "/staging")) {
        err = "创建目录失败: " + cfg_.server.storage_dir + "/staging (" + std::to_string(errno) + ")";
        return false;
    }
    return true;
}

std::string ArchiveService::final_rel_path(const dicom::DicomInfo &info) const {
    // 库里存相对 storage_dir 的路径（studyUID/seriesUID/sopUID.dcm）：
    // 换存储介质/挂载点不需要刷库；归档服务与消费端各自拼自己的根目录
    return info.study_instance_uid + "/" + info.series_instance_uid + "/" +
           info.sop_instance_uid + ".dcm";
}

ImportResult ArchiveService::import_image(const char *data, size_t size) {
    ImportResult res;
    res.http_status = 500;
    res.status_word = "FAILED";
    res.instance_id = 0;

    std::string err;
    if (!ensure_dirs(err)) {
        res.detail = err;
        return res;
    }

    // ① 落盘 staging（UUID 文件名：并发导入互不覆盖，也是续传场景的锚点）
    std::string staging = cfg_.server.storage_dir + "/staging/" + util::gen_uuid_v4() + ".part";
    std::FILE *f = std::fopen(staging.c_str(), "wb");
    if (f == NULL) {
        res.detail = "无法创建 staging 文件: " + staging;
        return res;
    }
    size_t written = std::fwrite(data, 1, size, f);
    int flush_err = std::fflush(f) != 0;
    std::fclose(f);
    if (written != size || flush_err) {
        ::unlink(staging.c_str());
        res.detail = "staging 文件写入不完整";
        return res;
    }

    // ② 解析（整文件读入：解析器遇像素数据即停，实际只扫元数据段）
    std::FILE *pf = std::fopen(staging.c_str(), "rb");
    if (pf == NULL) {
        ::unlink(staging.c_str());
        res.detail = "staging 文件复读失败";
        return res;
    }
    std::vector<char> buf(size);
    if (size > 0 && std::fread(buf.data(), 1, size, pf) != size) {
        std::fclose(pf);
        ::unlink(staging.c_str());
        res.detail = "staging 文件读取不完整";
        return res;
    }
    std::fclose(pf);

    dicom::DicomInfo info;
    dicom::ParseResult pr = dicom::parse_dicom(buf.data(), buf.size(), info, err);
    if (pr != dicom::PARSE_OK) {
        ::unlink(staging.c_str());
        res.http_status = 400;
        res.detail = "DICOM 解析失败 [" + std::string(dicom::parse_result_name(pr)) + "]: " + err;
        return res;
    }

    // ③ 分块流式 SHA-256（内存占用与文件大小无关）
    util::FileHash fh = util::sha256_file(staging.c_str());
    if (!fh.ok) {
        ::unlink(staging.c_str());
        res.detail = "SHA-256 计算失败: " + fh.err;
        return res;
    }

    // ④ 事务一：幂等决策树
    BeginResult b = dao_.begin_archive(info, fh.hex, fh.size);
    if (b.decision == IMPORT_DB_ERROR) {
        ::unlink(staging.c_str());
        res.detail = b.err;
        return res;
    }
    if (b.decision == IMPORT_DUPLICATE) {
        ::unlink(staging.c_str()); // 重复导入：staging 副本无价值，删除
        res.http_status = 200;
        res.status_word = "DUPLICATE";
        res.instance_id = b.instance_id;
        res.detail = "同 SOPInstanceUID 同哈希，幂等返回既有归档";
        return res;
    }
    if (b.decision == IMPORT_CONFLICT) {
        ::unlink(staging.c_str());
        res.http_status = 409;
        res.status_word = "CONFLICT";
        res.instance_id = b.instance_id;
        res.detail = "同 SOPInstanceUID 但内容哈希不同，拒绝覆盖";
        return res;
    }

    // ⑤ rename 到最终路径（staging 与最终同在 storage_dir 下，rename 原子）
    std::string rel = final_rel_path(info);                 // 相对 storage_dir 的库内路径
    std::string final_abs = cfg_.server.storage_dir + "/" + rel;
    std::string dir = final_abs.substr(0, final_abs.rfind('/'));
    if (!mkdir_p(dir)) {
        ::unlink(staging.c_str());
        res.instance_id = b.instance_id;
        res.detail = "创建对象目录失败: " + dir;
        return res;
    }
    if (::rename(staging.c_str(), final_abs.c_str()) != 0) {
        ::unlink(staging.c_str());
        res.instance_id = b.instance_id;
        res.detail = "rename 到最终路径失败 (" + std::to_string(errno) + "): " + final_abs;
        return res;
    }

    // ⑥ 事务二：ARCHIVED + backup_event（同事务）。失败则清理孤儿文件
    std::string task_id;
    if (!dao_.finish_archive(b.instance_id, rel, task_id, err)) {
        ::unlink(final_abs.c_str());
        res.instance_id = b.instance_id;
        res.detail = "归档事务二失败(已回滚，对象文件已清理): " + err;
        return res;
    }

    res.http_status = 201;
    res.status_word = "ARCHIVED";
    res.instance_id = b.instance_id;
    res.task_id = task_id;
    res.detail = (b.decision == IMPORT_RESUME) ? "中断对象续传完成" : "新实例归档完成";
    LOG_INFO << (b.decision == IMPORT_RESUME ? "[续传] " : "[归档] ") << info.sop_instance_uid
             << " sha256=" << fh.hex.substr(0, 12) << "... size=" << fh.size
             << " task=" << task_id << " path=" << rel;
    return res;
}

} // namespace archive
} // namespace pacs
