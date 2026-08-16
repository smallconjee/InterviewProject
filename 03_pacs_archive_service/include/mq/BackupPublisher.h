// ============================================================================
// BackupPublisher.h — 备份消息发布端（裸 rabbitmq-c + Publisher Confirm）
//
// 为什么不用 SimpleAmqpClient 发消息：2.5.1 没有暴露 confirm API，
// 而"broker 确认收到"是本地消息表闭环的关键一环（简历 bullet 4），
// 所以发布端用 rabbitmq-c 原生实现 confirm.select + 等待 basic.ack；
// 消费端（重连接友好、无需 confirm）继续用 SimpleAmqpClient。
//
// 拓扑：exchange pacs.backup (direct) --backup--> queue pacs.backup.queue
//       queue 参数带 x-dead-letter-exchange=pacs.backup.dlx (fanout) --DLQ
// 消息体：JSON {"task_id":"uuid","instance_id":N}，持久化(delivery_mode=2)
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "common/Config.h"

namespace pacs {
namespace mq {

class BackupPublisher {
public:
    BackupPublisher();
    ~BackupPublisher();

    // 连接 + 声明拓扑 + 开启 confirm 模式
    bool init(const common::RabbitMqConfig &cfg, std::string &err);

    // 发布一条备份消息并等待 broker confirm（3 秒超时算失败）
    // 成功后调用方应把 backup_event 置为 PUBLISHED
    bool publish(const std::string &task_id, uint64_t instance_id, std::string &err);

    void close();

private:
    bool declare_topology(std::string &err);

    void *conn_;     // amqp_connection_state_t（头文件不暴露 rabbitmq 头）
    int channel_;
    bool inited_;
};

} // namespace mq
} // namespace pacs
