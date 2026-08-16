// ============================================================================
// ReportParser.h — RIS 检查报告 XML 解析（TinyXML2）
//
// 报告 schema（项目约定，与 RIS 导出方对接的契约）：
// <report>
//   <report_id>RPT-2026-0001</report_id>            必填，唯一
//   <study_instance_uid>1.2.826...</study_instance_uid> 可选：有才关联归档影像
//   <patient><patient_id/><patient_name/><sex/><birth_date/></patient>
//   <exam><exam_date/><modality/><exam_part/><hospital/><doctor/></exam>
//   <diagnosis><conclusion/> 必填 <description/> 可选 <impression/> 可选</diagnosis>
// </report>
// 解析策略：坏文件不致命——缺必填/格式错记入 skipped 原因，离线构建时跳过并统计
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace ris {
namespace report {

struct ReportRecord {
    std::string report_id;
    std::string study_instance_uid; // 可空：数据源未提供则不关联影像
    std::string patient_id;
    std::string patient_name;
    std::string sex;
    std::string exam_date; // YYYY-MM-DD
    std::string modality;  // CT/MR/DR
    std::string exam_part;
    std::string doctor;
    std::string conclusion;  // 诊断结论（索引主字段）
    std::string description; // 影像所见（索引副字段）
};

// 解析结果：ok=false 时 reason 带原因（字段缺失/格式错误/非 XML）
struct ParseOutcome {
    bool ok;
    std::string reason;
};

ParseOutcome parse_report_file(const std::string &path, ReportRecord &out);

// 遍历目录下全部 .xml 并解析；bad 列表带 (文件名, 原因) 供构建日志/质量报告
struct DirScanResult {
    std::vector<ReportRecord> records;
    std::vector<std::pair<std::string, std::string>> bad; // 文件名 -> 拒绝原因
};
DirScanResult parse_report_dir(const std::string &dir);

} // namespace report
} // namespace ris
