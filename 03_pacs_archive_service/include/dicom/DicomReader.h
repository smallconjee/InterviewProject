// ============================================================================
// DicomReader.h — 手写轻量 DICOM Part-10 解析器（只读本项目需要的 tag）
//
// 为什么不用 DCMTK（面试必问）：DCMTK 依赖重、API 面广，而本项目只需要从
// CT/MR/DR 文件中提取四层元数据；自写 ~300 行可控、可讲、可调试。
// 支持面刻意收窄（诚实的边界）：
//   ✓ 文件元组（group 0002，标准规定恒为显式 VR 小端）
//   ✓ 显式 VR 小端 / 隐式 VR 小端 两种传输语法的数据集
//   ✗ Big Endian 传输语法（2008 年起废弃）→ 明确报错拒绝
//   ✗ 像素数据内部（压缩/封装格式）→ 解析到 (7FE0,0010) 即停，理由见实现
// 依赖宿主为小端架构（x86_64/aarch64 容器均满足），跨大端平台需改造 rd16/rd32
// ============================================================================
#pragma once

#include <cstddef>
#include <string>

namespace pacs {
namespace dicom {

// 从一个 DICOM 文件提取出的四层元数据（括号内为 DICOM tag）
struct DicomInfo {
    std::string transfer_syntax_uid;  // (0002,0010) 传输语法，决定数据集编码方式
    std::string sop_class_uid;        // (0008,0016)
    std::string sop_instance_uid;     // (0008,0018) 幂等键
    std::string study_instance_uid;   // (0020,000D) 报告检索侧关联影像的锚点
    std::string series_instance_uid;  // (0020,000E)
    std::string patient_id;           // (0010,0020)
    std::string patient_name;         // (0010,0010)
    std::string issuer;               // (0010,0021) 患者来源，与 PatientID 组成复合身份
    std::string birth_date;           // (0010,0030) DICOM 日期格式 YYYYMMDD
    std::string sex;                  // (0010,0040) M/F/O
    std::string modality;             // (0008,0060) CT/MR/DR
    std::string study_date;           // (0008,0020) YYYYMMDD
    std::string accession_number;     // (0008,0050) 检查号
    std::string study_description;    // (0008,1030)
    int series_number = -1;           // (0020,0011) IS 整数字符串，-1 表示缺失
    int instance_number = -1;         // (0020,0013)
};

enum ParseResult {
    PARSE_OK = 0,
    PARSE_NOT_DICOM,          // 不是 DICOM 文件（preamble/DICM 魔数不符）
    PARSE_TRUNCATED,          // 文件不完整（导入中断的典型形态）
    PARSE_UNSUPPORTED_TS,     // 不支持的传输语法（Big Endian 等）
    PARSE_MISSING_REQUIRED,   // 缺少必需 tag（四层 UID / PatientID）
    PARSE_BAD_FORMAT,         // 结构非法（长度越界、VR 异常等）
};

// 解析入口：data/size 为文件完整内容（读取一次、内存中解析）
ParseResult parse_dicom(const char *data, size_t size, DicomInfo &out, std::string &err);

// 结果码转可读名（日志/命令行工具用）
const char *parse_result_name(ParseResult r);

} // namespace dicom
} // namespace pacs
