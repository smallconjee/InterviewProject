#include "mq/BackupPublisher.h"

#include <cstdio>
#include <cstring>

#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>

#include "common/Logger.h"

namespace pacs {
namespace mq {

namespace {

// rabbitmq-c 惯用的错误检查宏（改写为函数风格便于日志）
bool amqp_ok(amqp_rpc_reply_t r, const char *what, std::string &err) {
    if (r.reply_type == AMQP_RESPONSE_NORMAL) return true;
    err = std::string(what) + " 失败: reply_type=" + std::to_string(r.reply_type);
    return false;
}

const char *EXCHANGE = "pacs.backup";
const char *QUEUE = "pacs.backup.queue";
const char *ROUTING_KEY = "backup";
const char *DLX = "pacs.backup.dlx";
const char *DLQ = "pacs.backup.dead";

} // namespace

BackupPublisher::BackupPublisher() : conn_(NULL), channel_(1), inited_(false) {}

BackupPublisher::~BackupPublisher() { close(); }

void BackupPublisher::close() {
    if (conn_ != NULL) {
        amqp_connection_close(reinterpret_cast<amqp_connection_state_t>(conn_),
                              AMQP_REPLY_SUCCESS);
        amqp_destroy_connection(reinterpret_cast<amqp_connection_state_t>(conn_));
        conn_ = NULL;
    }
    inited_ = false;
}

bool BackupPublisher::init(const common::RabbitMqConfig &cfg, std::string &err) {
    amqp_connection_state_t conn = amqp_new_connection();
    amqp_socket_t *sock = amqp_tcp_socket_new(conn);
    if (sock == NULL) {
        err = "创建 TCP socket 失败";
        amqp_destroy_connection(conn);
        return false;
    }
    struct timeval tv = {5, 0}; // 连接超时 5s
    if (amqp_socket_open_noblock(sock, cfg.host.c_str(), cfg.port, &tv) != AMQP_STATUS_OK) {
        err = "连接 RabbitMQ 失败: " + cfg.host + ":" + std::to_string(cfg.port);
        amqp_destroy_connection(conn);
        return false;
    }
    amqp_rpc_reply_t r = amqp_login(conn, "/", 0, 131072, 5, AMQP_SASL_METHOD_PLAIN,
                                    cfg.user.c_str(), cfg.password.c_str());
    if (!amqp_ok(r, "amqp_login", err)) {
        amqp_destroy_connection(conn);
        return false;
    }
    amqp_channel_open(conn, channel_);
    if (!amqp_ok(amqp_get_rpc_reply(conn), "amqp_channel_open", err)) {
        amqp_destroy_connection(conn);
        return false;
    }

    conn_ = conn;
    if (!declare_topology(err)) {
        close();
        return false;
    }

    // ★ 开启 publisher confirm：此后 broker 对每条消息回 basic.ack/nack
    amqp_confirm_select(conn, channel_);
    if (!amqp_ok(amqp_get_rpc_reply(conn), "confirm.select", err)) {
        close();
        return false;
    }
    inited_ = true;
    LOG_INFO << "BackupPublisher 就绪: " << cfg.host << ":" << cfg.port
             << " exchange=" << EXCHANGE << " queue=" << QUEUE << " (confirm 模式)";
    return true;
}

bool BackupPublisher::declare_topology(std::string &err) {
    amqp_connection_state_t conn = reinterpret_cast<amqp_connection_state_t>(conn_);

    amqp_bytes_t ex = amqp_cstring_bytes(EXCHANGE);
    amqp_bytes_t q = amqp_cstring_bytes(QUEUE);
    amqp_bytes_t rk = amqp_cstring_bytes(ROUTING_KEY);

    // 1) 主交换机：direct，持久化
    amqp_exchange_declare(conn, channel_, ex, amqp_cstring_bytes("direct"), 0, 0, 0, 0,
                          amqp_empty_table);
    if (!amqp_ok(amqp_get_rpc_reply(conn), "exchange_declare", err)) return false;

    // 2) 死信交换机：fanout
    amqp_exchange_declare(conn, channel_, amqp_cstring_bytes(DLX),
                          amqp_cstring_bytes("fanout"), 0, 0, 0, 0, amqp_empty_table);
    if (!amqp_ok(amqp_get_rpc_reply(conn), "dlx_declare", err)) return false;

    // 3) 主队列：持久化，绑定死信交换机（消费端 nack(requeue=false) 时进 DLQ）
    amqp_field_value_t dlx_val;
    dlx_val.kind = AMQP_FIELD_KIND_UTF8;
    dlx_val.value.bytes = amqp_cstring_bytes(DLX);
    amqp_table_entry_t dlx_arg[1];
    dlx_arg[0].key = amqp_cstring_bytes("x-dead-letter-exchange");
    dlx_arg[0].value = dlx_val;
    amqp_table_t q_args;
    q_args.num_entries = 1;
    q_args.entries = dlx_arg;
    amqp_queue_declare(conn, channel_, q, 0, 1, 0, 0, q_args);
    if (!amqp_ok(amqp_get_rpc_reply(conn), "queue_declare", err)) return false;

    // 4) 死信队列 + 绑定
    amqp_queue_declare(conn, channel_, amqp_cstring_bytes(DLQ), 0, 1, 0, 0, amqp_empty_table);
    if (!amqp_ok(amqp_get_rpc_reply(conn), "dlq_declare", err)) return false;
    amqp_queue_bind(conn, channel_, amqp_cstring_bytes(DLQ),
                    amqp_cstring_bytes(DLX), amqp_cstring_bytes(""), amqp_empty_table);
    if (!amqp_ok(amqp_get_rpc_reply(conn), "dlq_bind", err)) return false;

    // 5) 主队列绑定主交换机
    amqp_queue_bind(conn, channel_, q, ex, rk, amqp_empty_table);
    if (!amqp_ok(amqp_get_rpc_reply(conn), "queue_bind", err)) return false;
    return true;
}

bool BackupPublisher::publish(const std::string &task_id, uint64_t instance_id,
                              std::string &err) {
    if (!inited_) {
        err = "publisher 未初始化";
        return false;
    }
    amqp_connection_state_t conn = reinterpret_cast<amqp_connection_state_t>(conn_);

    char body[160];
    int n = std::snprintf(body, sizeof(body),
                          "{\"task_id\":\"%s\",\"instance_id\":%llu}",
                          task_id.c_str(), (unsigned long long)instance_id);

    amqp_basic_properties_t props;
    props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
    props.content_type = amqp_cstring_bytes("application/json");
    props.delivery_mode = 2; // 持久化消息，broker 重启不丢

    if (amqp_basic_publish(conn, channel_, amqp_cstring_bytes(EXCHANGE),
                           amqp_cstring_bytes(ROUTING_KEY), 0, 0, &props,
                           amqp_bytes_t{static_cast<size_t>(n), reinterpret_cast<uint8_t *>(body)}) != 0) {
        err = "amqp_basic_publish 失败";
        return false;
    }

    // 等待 broker confirm：basic.ack(60,80) 或 basic.nack(60,120)
    struct timeval tv = {3, 0};
    for (;;) {
        amqp_frame_t frame;
        int rc = amqp_simple_wait_frame_noblock(conn, &frame, &tv);
        if (rc != AMQP_STATUS_OK) {
            err = "等待 publisher confirm 超时/失败 rc=" + std::to_string(rc);
            return false;
        }
        if (frame.frame_type != AMQP_FRAME_METHOD) continue;
        // method.id 是 (class_id<<16|method_id) 打包的方法号，直接比对常量
        if (frame.payload.method.id == AMQP_BASIC_ACK_METHOD) {
            return true; // broker 已落盘（队列持久化 + 消息持久化）
        }
        if (frame.payload.method.id == AMQP_BASIC_NACK_METHOD) {
            err = "broker 返回 nack（消息被拒收）";
            return false;
        }
    }
}

} // namespace mq
} // namespace pacs
