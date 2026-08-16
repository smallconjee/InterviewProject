#include "protocol/TlvCodec.h"

#include <cstring>

namespace ris {
namespace protocol {

std::string encode_frame(uint8_t type, const std::string &payload) {
    std::string out;
    out.reserve(6 + payload.size());
    out.push_back(static_cast<char>(type));
    out.push_back('\0'); // flags 保留
    // 长度写为网络字节序（大端）：高字节在前
    uint32_t len = static_cast<uint32_t>(payload.size());
    out.push_back(static_cast<char>((len >> 24) & 0xFF));
    out.push_back(static_cast<char>((len >> 16) & 0xFF));
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
    out += payload;
    return out;
}

void TlvDecoder::feed(const char *data, size_t len) {
    buf_.append(data, len);
}

void TlvDecoder::feed(const std::string &s) {
    buf_ += s;
}

DecodeStatus TlvDecoder::decode(Frame &frame) {
    // 1) 头部 6 字节都不齐：半帧，等下一次数据
    if (buf_.size() < 6) {
        return DECODE_PARTIAL;
    }

    const uint8_t type = static_cast<uint8_t>(buf_[0]);

    // 2) 网络字节序（大端）读长度：小端宿主必须手动拼，不能 memcpy
    uint32_t len = 0;
    len |= static_cast<uint32_t>(static_cast<uint8_t>(buf_[2])) << 24;
    len |= static_cast<uint32_t>(static_cast<uint8_t>(buf_[3])) << 16;
    len |= static_cast<uint32_t>(static_cast<uint8_t>(buf_[4])) << 8;
    len |= static_cast<uint32_t>(static_cast<uint8_t>(buf_[5]));

    // 3) 合法性检查顺序：先类型后长度（非法类型不需要知道长度即可裁决）
    if (type != TYPE_SEARCH_REQ && type != TYPE_SUGGEST_REQ &&
        type != TYPE_SEARCH_RESP && type != TYPE_SUGGEST_RESP && type != TYPE_ERROR) {
        // 跳过这一帧的代价未知（类型非法时长度字段不可信），
        // 唯一安全的做法是要求断连——由调用方处理
        return DECODE_BAD_TYPE;
    }
    if (len > MAX_FRAME_PAYLOAD) {
        return DECODE_TOO_LONG; // 超长帧：内存保护，连接应断开
    }

    // 4) 体不完整：半帧
    if (buf_.size() < 6 + len) {
        return DECODE_PARTIAL;
    }

    // 5) 完整帧：消费掉（一次性 erase，剩余留给下一帧）
    frame.type = type;
    frame.flags = static_cast<uint8_t>(buf_[1]);
    frame.payload.assign(buf_, 6, len);
    buf_.erase(0, 6 + len);
    return DECODE_OK;
}

} // namespace protocol
} // namespace ris
