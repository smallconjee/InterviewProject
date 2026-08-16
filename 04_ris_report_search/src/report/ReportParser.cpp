#include "report/ReportParser.h"

#include <cstring>
#include <dirent.h>

#include <tinyxml2.h>

namespace ris {
namespace report {

namespace {

// 必填字段缺失的统一出口：返回原因，不抛异常
ParseOutcome reject(const std::string &reason) {
    ParseOutcome o;
    o.ok = false;
    o.reason = reason;
    return o;
}

// 取子元素文本（缺失返回空串；TinyXML2 的 GetText 对空元素返回 NULL）
std::string child_text(const tinyxml2::XMLElement *parent, const char *name) {
    const tinyxml2::XMLElement *e = parent->FirstChildElement(name);
    if (e == NULL) return "";
    const char *t = e->GetText();
    return t != NULL ? std::string(t) : std::string();
}

} // namespace

ParseOutcome parse_report_file(const std::string &path, ReportRecord &out) {
    tinyxml2::XMLDocument doc;
    // 不支持的声明/编码错误也算解析失败（XML_ERROR_* 全部非 0）
    if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        return reject(std::string("XML 解析失败: ") + doc.ErrorStr());
    }
    const tinyxml2::XMLElement *root = doc.FirstChildElement("report");
    if (root == NULL) {
        return reject("缺少 <report> 根元素");
    }

    out.report_id = child_text(root, "report_id");
    if (out.report_id.empty()) {
        return reject("缺少必填 report_id");
    }
    // 可选字段：数据源提供才建立报告-影像关联（简历 bullet 1 的边界处理）
    out.study_instance_uid = child_text(root, "study_instance_uid");

    const tinyxml2::XMLElement *pat = root->FirstChildElement("patient");
    if (pat != NULL) {
        out.patient_id = child_text(pat, "patient_id");
        out.patient_name = child_text(pat, "patient_name");
        out.sex = child_text(pat, "sex");
        // birth_date 不参与索引，跳过
    }
    if (out.patient_id.empty()) {
        return reject("缺少必填 patient/patient_id");
    }

    const tinyxml2::XMLElement *exam = root->FirstChildElement("exam");
    if (exam != NULL) {
        out.exam_date = child_text(exam, "exam_date");
        out.modality = child_text(exam, "modality");
        out.exam_part = child_text(exam, "exam_part");
        out.doctor = child_text(exam, "doctor");
    }

    const tinyxml2::XMLElement *diag = root->FirstChildElement("diagnosis");
    if (diag == NULL) {
        return reject("缺少 <diagnosis> 节点");
    }
    out.conclusion = child_text(diag, "conclusion");
    if (out.conclusion.empty()) {
        return reject("缺少必填 diagnosis/conclusion");
    }
    out.description = child_text(diag, "description");

    ParseOutcome o;
    o.ok = true;
    return o;
}

DirScanResult parse_report_dir(const std::string &dir) {
    DirScanResult result;
    DIR *d = ::opendir(dir.c_str());
    if (d == NULL) {
        result.bad.push_back(std::make_pair(dir, "无法打开目录"));
        return result;
    }
    struct dirent *ent;
    while ((ent = ::readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = std::strlen(name);
        if (len < 5 || std::strcmp(name + len - 4, ".xml") != 0) {
            continue;
        }
        std::string path = dir + "/" + name;
        ReportRecord rec;
        ParseOutcome o = parse_report_file(path, rec);
        if (o.ok) {
            result.records.push_back(rec);
        } else {
            result.bad.push_back(std::make_pair(name, o.reason));
        }
    }
    ::closedir(d);
    return result;
}

} // namespace report
} // namespace ris
