// ============================================================================
// RisTlvClient.cpp — TLV 消息编解码 + 客户端任务工厂实现
//
// 实现参照 workflow 官方 tutorial-10（encode 返回 iovec 个数、append 增量
// 状态机）；帧格式对齐 04 侧 TlvCodec.cpp（1B type + 1B flags + 4B 大端
// length + payload），两端字节序处理手法一致：小端宿主上手动拼字节，
// 禁止对长度字段直接 memcpy/强转。
// ============================================================================
#include "risproxy/RisTlvClient.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <stdlib.h>

#include "workflow/WFGlobal.h"

namespace pacs {
namespace risproxy {

// ---------- 序列化（请求方向） ----------

int RisTlvMessage::encode(struct iovec vectors[], int max) {
    // 发送侧内存保护：查询串超长直接报错（正常查询串远小于 1MB）
    if (value_.size() > RIS_MAX_FRAME_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    uint32_t len = static_cast<uint32_t>(value_.size());
    head_[0] = static_cast<char>(type_);
    head_[1] = static_cast<char>(flags_);
    // 长度按网络字节序（大端）写入：高字节在前
    head_[2] = static_cast<char>((len >> 24) & 0xFF);
    head_[3] = static_cast<char>((len >> 16) & 0xFF);
    head_[4] = static_cast<char>((len >> 8) & 0xFF);
    head_[5] = static_cast<char>(len & 0xFF);

    vectors[0].iov_base = head_;
    vectors[0].iov_len = 6;
    vectors[1].iov_base = const_cast<char *>(value_.data());
    vectors[1].iov_len = value_.size();
    return 2; // 使用的 iovec 个数
}

// ---------- 反序列化（响应方向，增量状态机） ----------

int RisTlvMessage::append(const void *buf, size_t *size) {
    const char *p = static_cast<const char *>(buf);
    size_t left = *size;
    size_t consumed = 0;

    // 1) 头部 6 字节未凑齐：先补头，仍不够则本轮结束（半帧）
    if (head_received_ < 6) {
        size_t need = 6 - head_received_;
        size_t take = need < left ? need : left;
        ::memcpy(head_ + head_received_, p, take);
        head_received_ += take;
        p += take;
        left -= take;
        consumed += take;
        if (head_received_ < 6) {
            *size = consumed;
            return 0;
        }

        // 头凑齐：解析 type/flags 与大端 length
        type_ = static_cast<uint8_t>(head_[0]);
        flags_ = static_cast<uint8_t>(head_[1]);
        uint32_t len = 0;
        len |= static_cast<uint32_t>(static_cast<uint8_t>(head_[2])) << 24;
        len |= static_cast<uint32_t>(static_cast<uint8_t>(head_[3])) << 16;
        len |= static_cast<uint32_t>(static_cast<uint8_t>(head_[4])) << 8;
        len |= static_cast<uint32_t>(static_cast<uint8_t>(head_[5]));

        // 客户端只接受三种响应帧（与 04 服务端的合法类型集合保持一致）
        if (type_ != RIS_TYPE_SEARCH_RESP && type_ != RIS_TYPE_SUGGEST_RESP &&
            type_ != RIS_TYPE_ERROR) {
            errno = EBADMSG;
            return -1;
        }
        if (len > size_limit) { // size_limit 由 set_size_limit 控制
            errno = EMSGSIZE;
            return -1;
        }
        body_size_ = len;
        value_.clear();
        value_.reserve(len);
    }

    // 2) 收 payload：收够 body_size_ 字节即完整
    size_t need = body_size_ - value_.size();
    size_t take = need < left ? need : left;
    value_.append(p, take);
    consumed += take;
    *size = consumed;
    if (value_.size() < body_size_) {
        return 0; // 半帧，等下一批数据
    }
    return 1; // 完整帧（多余字节属于下一帧，由框架按 *size 语义保留）
}

// ---------- 移动语义（std::string 成员自动搬运，仅同步解码状态） ----------

RisTlvMessage::RisTlvMessage(RisTlvMessage &&msg)
    : protocol::ProtocolMessage(std::move(msg)),
      type_(msg.type_),
      flags_(msg.flags_),
      head_received_(msg.head_received_),
      body_size_(msg.body_size_),
      value_(std::move(msg.value_)) {
    ::memcpy(head_, msg.head_, sizeof(head_));
    msg.head_received_ = 0;
    msg.body_size_ = 0;
}

RisTlvMessage &RisTlvMessage::operator=(RisTlvMessage &&msg) {
    if (this != &msg) {
        *static_cast<protocol::ProtocolMessage *>(this) =
            std::move(static_cast<protocol::ProtocolMessage &>(msg));
        type_ = msg.type_;
        flags_ = msg.flags_;
        ::memcpy(head_, msg.head_, sizeof(head_));
        head_received_ = msg.head_received_;
        body_size_ = msg.body_size_;
        value_ = std::move(msg.value_);
        msg.head_received_ = 0;
        msg.body_size_ = 0;
    }
    return *this;
}

// ---------- 地址解析与任务工厂 ----------

void resolve_ris_addr(std::string &host, unsigned short &port) {
    host = "127.0.0.1"; // 默认：PACS 与 RIS 同机部署（dev container 内）
    port = 9090;
    const char *addr = ::getenv("PACS_RIS_ADDR");
    if (addr != NULL && ::strchr(addr, ':') != NULL) {
        std::string a(addr);
        size_t pos = a.rfind(':');
        long p = ::atol(a.c_str() + pos + 1);
        if (p > 0 && p <= 65535) {
            host = a.substr(0, pos);
            port = static_cast<unsigned short>(p);
        }
    }
}

RisTlvTask *create_ris_task(const std::string &host, unsigned short port,
                            uint8_t frame_type, const std::string &payload,
                            RisTlvCallback cb) {
    RisTlvTask *task = WFNetworkTaskFactory<RisTlvMessage, RisTlvMessage>::
        create_client_task(TT_TCP, host, port, 1, std::move(cb));
    // 查询幂等，失败重试 1 次（连接建立失败是瞬时错误的最常见形态）
    task->set_keep_alive(60 * 1000); // 毫秒；连接由 workflow 池化复用
    task->get_req()->set_type(frame_type);
    task->get_req()->set_value(payload);
    task->get_resp()->set_size_limit(RIS_MAX_FRAME_PAYLOAD);
    return task;
}

} // namespace risproxy
} // namespace pacs
