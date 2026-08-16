// ============================================================================
// DicomReader.cpp — DICOM Part-10 解析实现
//
// 文件结构（自顶向下）：
//   [128B preamble][“DICM”魔数][文件元组(0002,恒为显式VR小端)][数据集(编码由传输语法决定)]
//
// 关键策略（面试讲解点）：
//   1. 遇到像素数据 (7FE0,0010) 立即停止——Part-10 要求数据集内 tag 升序，
//      我们要的元数据 tag 数值上都小于 7FE0，像素数据（文件体积的 99%）零扫描
//   2. 未定长 SQ 用"深度计数 + 递归跳过"处理，不解析序列内部内容
//   3. 所有读指针操作都带越界检查，任何截断/越界统一报 TRUNCATED/BAD_FORMAT，
//      绝不越界读——解析不可信输入是安全底线
// ============================================================================
#include "dicom/DicomReader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "common/Logger.h"

namespace pacs {
namespace dicom {

namespace {

// ---------- 只读游标：所有读取都做边界检查 ----------

struct Cursor {
    const char *p;
    const char *end;
};

// 依赖小端宿主直接 memcpy 读数值（x86_64/aarch64 容器均满足；见头文件说明）
bool rd_u16(Cursor &c, uint16_t &v) {
    if (c.end - c.p < 2) return false;
    std::memcpy(&v, c.p, 2);
    c.p += 2;
    return true;
}

bool rd_u32(Cursor &c, uint32_t &v) {
    if (c.end - c.p < 4) return false;
    std::memcpy(&v, c.p, 4);
    c.p += 4;
    return true;
}

bool read_tag(Cursor &c, uint32_t &tag) {
    uint16_t group = 0, elem = 0;
    if (!rd_u16(c, group) || !rd_u16(c, elem)) return false;
    tag = (static_cast<uint32_t>(group) << 16) | elem;
    return true;
}

bool skip_bytes(Cursor &c, uint32_t n) {
    if (n > static_cast<uint32_t>(c.end - c.p)) return false;
    c.p += n;
    return true;
}

// ---------- 本项目关心的 tag（group<<16 | element） ----------

const uint32_t TAG_XFER_SYNTAX      = 0x00020010;
const uint32_t TAG_MEDIA_SOP_UID    = 0x00020003;
const uint32_t TAG_SOP_CLASS_UID    = 0x00080016;
const uint32_t TAG_SOP_INSTANCE_UID = 0x00080018;
const uint32_t TAG_STUDY_DATE       = 0x00080020;
const uint32_t TAG_ACCESSION        = 0x00080050;
const uint32_t TAG_MODALITY         = 0x00080060;
const uint32_t TAG_STUDY_DESC       = 0x00081030;
const uint32_t TAG_SERIES_DESC      = 0x0008103E;
const uint32_t TAG_PATIENT_NAME     = 0x00100010;
const uint32_t TAG_PATIENT_ID       = 0x00100020;
const uint32_t TAG_ISSUER           = 0x00100021;
const uint32_t TAG_BIRTH_DATE       = 0x00100030;
const uint32_t TAG_PATIENT_SEX      = 0x00100040;
const uint32_t TAG_STUDY_UID        = 0x0020000D;
const uint32_t TAG_SERIES_UID       = 0x0020000E;
const uint32_t TAG_SERIES_NUMBER    = 0x00200011;
const uint32_t TAG_INSTANCE_NUMBER  = 0x00200013;
const uint32_t TAG_PIXEL_DATA       = 0x7FE00010;

// 特殊标签（group FFFE）：项与各种结束符，编码上【没有 VR 字段】
const uint32_t TAG_ITEM             = 0xFFFEE000;
const uint32_t TAG_ITEM_DELIM       = 0xFFFEE00D;
const uint32_t TAG_SEQ_DELIM        = 0xFFFEE0DD;

const uint32_t UNDEFINED_LENGTH     = 0xFFFFFFFF;

// 值域裁剪：UI 用 \0 填充到偶数长度，PN/CS 等用空格；取出时去掉尾部填充
std::string trim_pad(const char *s, uint32_t n) {
    while (n > 0 && (s[n - 1] == '\0' || s[n - 1] == ' ')) --n;
    return std::string(s, n);
}

int parse_is(const char *s, uint32_t n) { // IS = 十进制整数字符串
    std::string t = trim_pad(s, n);
    return t.empty() ? -1 : std::atoi(t.c_str());
}

// 未定长 SQ：深度计数跳到对应的 (FFFE,E0DD)。项内若再嵌未定长 SQ 则递归。
bool skip_undefined_sequence(Cursor &c, bool explicit_vr);

// 按当前传输语法读一个元素的长度，游标停到 value 起始处
bool read_value_length(Cursor &c, bool explicit_vr, const uint32_t tag, uint32_t &len,
                       std::string &err) {
    if (!explicit_vr) {
        return rd_u32(c, len); // 隐式 VR：恒为 u32 长度，与 VR 无关
    }
    // group FFFE 的项/结束符没有 VR 字段
    if ((tag >> 16) == 0xFFFE) {
        return rd_u32(c, len);
    }
    char vr[2];
    if (c.end - c.p < 2) {
        err = "读取 VR 时越界";
        return false;
    }
    vr[0] = c.p[0];
    vr[1] = c.p[1];
    c.p += 2;
    // 长形式 VR（OB/OW/OF/OD/OL/SQ/UC/UR/UT/UN/SV/UV）：2 字节保留 + u32 长度
    const char *long_vrs[] = {"OB", "OW", "OF", "OD", "OL", "SQ", "UC", "UR", "UT", "UN", "SV", "UV"};
    bool is_long = false;
    for (size_t i = 0; i < sizeof(long_vrs) / sizeof(long_vrs[0]); ++i) {
        if (vr[0] == long_vrs[i][0] && vr[1] == long_vrs[i][1]) {
            is_long = true;
            break;
        }
    }
    if (is_long) {
        if (!skip_bytes(c, 2)) return false; // 2 字节保留位（恒 0）
        return rd_u32(c, len);
    }
    uint16_t short_len = 0;
    if (!rd_u16(c, short_len)) return false;
    len = short_len;
    return true;
}

// 解析数据集主体。explicit_vr 由传输语法决定。
bool parse_dataset(Cursor &c, bool explicit_vr, DicomInfo &out, std::string &err) {
    while (c.p < c.end) {
        uint32_t tag = 0;
        if (!read_tag(c, tag)) {
            err = "读取 tag 时越界";
            return false;
        }

        // 像素数据：Part-10 要求数据集内 tag 升序，元数据已全部读毕，就此打住。
        // 好处：几十 MB 的像素区零扫描（详见文件头注释）。
        if (tag == TAG_PIXEL_DATA) {
            return true;
        }

        uint32_t len = 0;
        if (!read_value_length(c, explicit_vr, tag, len, err)) {
            err = err.empty() ? "读取元素长度时越界" : err;
            return false;
        }

        if (len == UNDEFINED_LENGTH) {
            // 未定长只可能是 SQ（或封装像素数据，已在上面提前退出）
            if (!skip_undefined_sequence(c, explicit_vr)) {
                err = "未定长序列没有结束符（文件截断）";
                return false;
            }
            continue;
        }

        if (len > static_cast<uint32_t>(c.end - c.p)) {
            err = "元素长度越过文件末尾（文件截断）";
            return false;
        }

        // 只记录本项目关心的 tag，其余直接跳过
        switch (tag) {
            case TAG_SOP_CLASS_UID:    out.sop_class_uid = trim_pad(c.p, len); break;
            case TAG_SOP_INSTANCE_UID: out.sop_instance_uid = trim_pad(c.p, len); break;
            case TAG_STUDY_DATE:       out.study_date = trim_pad(c.p, len); break;
            case TAG_ACCESSION:        out.accession_number = trim_pad(c.p, len); break;
            case TAG_MODALITY:         out.modality = trim_pad(c.p, len); break;
            case TAG_STUDY_DESC:       out.study_description = trim_pad(c.p, len); break;
            case TAG_SERIES_DESC:      /* 序列描述暂不入库，跳过 */ break;
            case TAG_PATIENT_NAME:     out.patient_name = trim_pad(c.p, len); break;
            case TAG_PATIENT_ID:       out.patient_id = trim_pad(c.p, len); break;
            case TAG_ISSUER:           out.issuer = trim_pad(c.p, len); break;
            case TAG_BIRTH_DATE:       out.birth_date = trim_pad(c.p, len); break;
            case TAG_PATIENT_SEX:      out.sex = trim_pad(c.p, len); break;
            case TAG_STUDY_UID:        out.study_instance_uid = trim_pad(c.p, len); break;
            case TAG_SERIES_UID:       out.series_instance_uid = trim_pad(c.p, len); break;
            case TAG_SERIES_NUMBER:    out.series_number = parse_is(c.p, len); break;
            case TAG_INSTANCE_NUMBER:  out.instance_number = parse_is(c.p, len); break;
            default: break; // 不关心的 tag：长度已知，统一跳过
        }
        c.p += len;
    }
    return true; // 没有像素数据就自然结束，同样合法
}

bool skip_undefined_sequence(Cursor &c, bool explicit_vr) {
    int depth = 0; // 嵌套的未定长项深度
    while (c.p < c.end) {
        uint32_t tag = 0;
        if (!read_tag(c, tag)) return false;
        uint32_t len = 0;
        if (!rd_u32(c, len)) return false; // FFFE 组与隐式一样：u32 长度、无 VR

        if (tag == TAG_ITEM) {
            if (len == UNDEFINED_LENGTH) {
                ++depth;
            } else if (!skip_bytes(c, len)) {
                return false;
            }
        } else if (tag == TAG_ITEM_DELIM) {
            if (depth > 0) --depth;
        } else if (tag == TAG_SEQ_DELIM) {
            if (depth == 0) return true; // 本层序列结束
            --depth;
        } else {
            // 项内部的普通元素：按传输语法继续读，未定长则递归下钻。
            // 此处刚消费的 4 字节在显式 VR 下其实是 VR+短长度，先回退再重读
            c.p -= 4;
            std::string dummy_err;
            if (!read_value_length(c, explicit_vr, tag, len, dummy_err)) return false;
            if (len == UNDEFINED_LENGTH) {
                if (!skip_undefined_sequence(c, explicit_vr)) return false;
            } else if (!skip_bytes(c, len)) {
                return false;
            }
        }
    }
    return false; // 扫到文件尾也没等到结束符
}

} // namespace

ParseResult parse_dicom(const char *data, size_t size, DicomInfo &out, std::string &err) {
    // 1) preamble(128B) + "DICM" 魔数
    if (size < 132) {
        err = "文件不足 132 字节，连 preamble+魔数都放不下";
        return PARSE_NOT_DICOM;
    }
    if (std::memcmp(data + 128, "DICM", 4) != 0) {
        err = "偏移 128 处缺少 DICM 魔数";
        return PARSE_NOT_DICOM;
    }

    Cursor c = {data + 132, data + size};

    // 2) 文件元组：标准规定恒为显式 VR 小端，读到 group != 0002 为止
    bool ts_found = false;
    while (c.p < c.end) {
        uint32_t tag = 0;
        if (!read_tag(c, tag)) {
            err = "文件元组读取越界";
            return PARSE_TRUNCATED;
        }
        if ((tag >> 16) != 0x0002) {
            // 读过头了：把 tag 的 4 字节退回去，交给数据集解析
            c.p -= 4;
            break;
        }
        uint32_t len = 0;
        std::string e;
        if (!read_value_length(c, true, tag, len, e)) {
            err = "文件元组长度非法";
            return PARSE_TRUNCATED;
        }
        if (len > static_cast<uint32_t>(c.end - c.p)) {
            err = "文件元组越过文件末尾";
            return PARSE_TRUNCATED;
        }
        if (tag == TAG_XFER_SYNTAX) {
            out.transfer_syntax_uid = trim_pad(c.p, len);
            ts_found = true;
        } else if (tag == TAG_MEDIA_SOP_UID) {
            out.sop_instance_uid = trim_pad(c.p, len); // 元组里的副本，数据集缺失时兜底
        }
        c.p += len;
    }
    if (!ts_found) {
        err = "文件元组缺少传输语法 (0002,0010)";
        return PARSE_BAD_FORMAT;
    }

    // 3) 按传输语法决定数据集编码方式
    bool explicit_vr = false;
    if (out.transfer_syntax_uid == "1.2.840.10008.1.2") {
        explicit_vr = false; // 隐式 VR 小端（默认传输语法）
    } else if (out.transfer_syntax_uid == "1.2.840.10008.1.2.1" ||
               out.transfer_syntax_uid.rfind("1.2.840.10008.1.2.4", 0) == 0 ||
               out.transfer_syntax_uid.rfind("1.2.840.10008.1.2.5", 0) == 0) {
        // 显式 VR 小端；JPEG/JPEG-LS/JPEG2000 等压缩语法的【数据集】同样按显式小端编码，
        // 压缩只影响像素数据内部——而我们在 (7FE0,0010) 已提前退出，不受影响
        explicit_vr = true;
    } else if (out.transfer_syntax_uid == "1.2.840.10008.1.2.2") {
        err = "Big Endian 传输语法已废弃，本项目不支持";
        return PARSE_UNSUPPORTED_TS;
    } else {
        err = "未知传输语法: " + out.transfer_syntax_uid;
        return PARSE_UNSUPPORTED_TS;
    }

    // 4) 数据集主体
    if (!parse_dataset(c, explicit_vr, out, err)) {
        return PARSE_TRUNCATED;
    }

    // 5) 必需字段校验：四层 UID + PatientID（患者表复合唯一键的一部分）
    if (out.sop_instance_uid.empty() || out.study_instance_uid.empty() ||
        out.series_instance_uid.empty()) {
        err = "缺少必需 tag（SOP/Study/Series InstanceUID 至少其一为空）";
        return PARSE_MISSING_REQUIRED;
    }
    if (out.patient_id.empty()) {
        err = "缺少 PatientID (0010,0020)：患者身份 (PatientID,Issuer) 不允许为空";
        return PARSE_MISSING_REQUIRED;
    }
    return PARSE_OK;
}

const char *parse_result_name(ParseResult r) {
    switch (r) {
        case PARSE_OK:             return "OK";
        case PARSE_NOT_DICOM:      return "NOT_DICOM";
        case PARSE_TRUNCATED:      return "TRUNCATED";
        case PARSE_UNSUPPORTED_TS: return "UNSUPPORTED_TS";
        case PARSE_MISSING_REQUIRED: return "MISSING_REQUIRED";
        case PARSE_BAD_FORMAT:     return "BAD_FORMAT";
    }
    return "UNKNOWN";
}

} // namespace dicom
} // namespace pacs
