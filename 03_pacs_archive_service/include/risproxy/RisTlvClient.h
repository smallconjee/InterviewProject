// ============================================================================
// RisTlvClient.h — RIS 检索服务的 TLV 协议客户端（Web 控制台桥接）
//
// 职责：把 PACS 的 HTTP 请求桥接到 04_ris_report_search 的 TCP 自定义协议
//   （默认 127.0.0.1:9090），帧格式与 04 侧 TlvCodec 完全一致：
//   [type:1B][flags:1B 保留置0][length:4B 网络字节序][payload:length 字节]
//
// 关键设计决策：
//   - 基于 Sogou Workflow 的"用户自定义协议"机制（ProtocolMessage 派生 +
//     WFNetworkTaskFactory<T, T>::create_client_task，参照官方 tutorial-10），
//     HTTP handler 通过 SeriesWork 串联本任务、在回调里写 HTTP 响应——
//     全异步，不阻塞 wfrest 的 handler 线程
//   - set_keep_alive 让 workflow 连接池按 (host, port) 复用长连接，
//     与 scripts/test_ris_client.py 每查询新建连接的方式形成对比
//   - 响应帧 payload 本身就是 JSON 文本（04 侧手工拼装），客户端只校验
//     帧类型与长度，不解析内容、原样上交——避免一层多余的结构拷贝
//
// 已知限制：
//   - 只做"一请求一响应"，不支持单连接并发流水线（RIS 服务端也是同语义）
//   - 收到 ERROR 帧(0x7F)不重试，交还调用方按 502 语义处理
//   - RIS 未接入 Consul，地址解析只有 环境变量 > 默认值 两级
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "workflow/ProtocolMessage.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/Workflow.h"

namespace pacs {
namespace risproxy {

// 帧类型：与 04_ris_report_search/include/protocol/TlvCodec.h 的 FrameType 对齐
// （两份常量各自独立维护——跨进程共享头文件会引入目录耦合，故选择手工同步）
enum RisFrameType {
    RIS_TYPE_SEARCH_REQ   = 0x01, // payload: UTF-8 查询串
    RIS_TYPE_SUGGEST_REQ  = 0x02, // payload: UTF-8 查询串
    RIS_TYPE_SEARCH_RESP  = 0x11, // payload: JSON 结果
    RIS_TYPE_SUGGEST_RESP = 0x12,
    RIS_TYPE_ERROR        = 0x7F, // payload: 错误说明 JSON
};

// 与 04 侧 MAX_FRAME_PAYLOAD 一致的接收上限（内存保护，超限报 EMSGSIZE）
static const uint32_t RIS_MAX_FRAME_PAYLOAD = 1 << 20; // 1MB

// TLV 消息：请求与响应同一 wire format，一类两用（教程同款用法）
class RisTlvMessage : public protocol::ProtocolMessage {
public:
    RisTlvMessage()
        : type_(0), flags_(0), head_received_(0), body_size_(0) {}

    // 移动构造/赋值：框架在重试等路径会移动消息对象（教程强烈建议实现）
    RisTlvMessage(RisTlvMessage &&msg);
    RisTlvMessage &operator=(RisTlvMessage &&msg);

    // ---- 请求方向（发送前设置）----
    void set_type(uint8_t t) { type_ = t; }
    void set_value(std::string v) { value_ = std::move(v); }

    // ---- 响应方向（任务回调里读取）----
    uint8_t type() const { return type_; }
    const std::string &value() const { return value_; }

private:
    // 序列化：6 字节头（type/flags/大端 length）+ payload，两个 iovec 零拷贝发送
    virtual int encode(struct iovec vectors[], int max);
    // 反序列化：增量解析。返回 0=半帧继续收，1=完整帧，-1=错误（置 errno）
    // size 出参语义：本次消费的字节数，剩余部分留给框架下次投递（官方推荐签名）
    virtual int append(const void *buf, size_t *size);

    uint8_t type_;
    uint8_t flags_;
    char head_[6];
    size_t head_received_; // 已收到的头部字节数（<6 表示头未凑齐）
    size_t body_size_;     // 由响应头解析出的 payload 长度
    std::string value_;    // payload（请求方向=查询串；响应方向=JSON 文本）
};

using RisTlvTask = WFNetworkTask<RisTlvMessage, RisTlvMessage>;
using RisTlvCallback = std::function<void(RisTlvTask *)>;

// 解析 RIS 服务地址：环境变量 PACS_RIS_ADDR（host:port）> 默认 127.0.0.1:9090
// 与 AuthGate 的 PACS_AUTH_ADDR 同一模式（env 直接指定，部署/调试最直接）
void resolve_ris_addr(std::string &host, unsigned short &port);

// 创建一次 TLV 查询任务：请求帧已填好、keep-alive 已设置，
// 调用方 series->push_back(task) 串联执行即可
RisTlvTask *create_ris_task(const std::string &host, unsigned short port,
                            uint8_t frame_type, const std::string &payload,
                            RisTlvCallback cb);

} // namespace risproxy
} // namespace pacs
