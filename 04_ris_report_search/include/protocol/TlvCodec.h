// ============================================================================
// TlvCodec.h — TLV 应用层协议 + 增量解码器（简历 RIS bullet 3）
//
// 帧格式（6 字节定长头 + 变长体）：
//   [type:1B][flags:1B 保留置0][length:4B 网络字节序][payload:length 字节]
//
// 增量解码（应对 TCP 流式的三种形态）：
//   半帧     —— 数据不足一帧：留在内部缓冲区等下一次数据（feed 追加）
//   连续多帧 —— 一次 feed 解出多帧
//   异常     —— 非法 type / 超长帧(>1MB)：返回 ERROR 帧，调用方应断开连接
// Length 按网络字节序解析：大端；x86/ARM 小端机上必须手动翻转（字节序考点）
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ris {
namespace protocol {

enum FrameType {
    TYPE_SEARCH_REQ = 0x01,  // payload: UTF-8 查询串
    TYPE_SUGGEST_REQ = 0x02, // payload: UTF-8 查询串
    TYPE_SEARCH_RESP = 0x11, // payload: JSON 结果
    TYPE_SUGGEST_RESP = 0x12,
    TYPE_ERROR = 0x7F,       // payload: 错误说明
};

enum DecodeStatus {
    DECODE_OK = 0,      // 解出一帧（frame 有效）
    DECODE_PARTIAL,     // 半帧：等更多数据（内部缓冲已保留）
    DECODE_BAD_TYPE,    // 非法类型：该帧已跳过，连接可继续
    DECODE_TOO_LONG,    // 超长帧：连接应断开
    DECODE_BAD_LENGTH,  // 长度头非法
};

static const uint32_t MAX_FRAME_PAYLOAD = 1 << 20; // 1MB 上限

struct Frame {
    uint8_t type;
    uint8_t flags;
    std::string payload;
};

// 编码：帧头 + payload 一次拼好（响应方向，整帧一次 send）
std::string encode_frame(uint8_t type, const std::string &payload);

// 增量解码器：一个 TCP 连接一个实例；feed 追加原始字节流
class TlvDecoder {
public:
    // 返回状态；DECODE_OK 时 frame 出参有效。循环调用直到非 OK。
    DecodeStatus decode(Frame &frame);

    void feed(const char *data, size_t len);
    void feed(const std::string &s);

    size_t pending() const { return buf_.size(); }

private:
    std::string buf_;
};

} // namespace protocol
} // namespace ris
