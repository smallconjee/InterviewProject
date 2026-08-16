// ============================================================================
// BackupDispatcher.h — 备份消息分发 + 补偿扫描（主服务进程内）
//
// 职责：
//   1. dispatch()：归档事务提交后立刻发布消息（快路径）
//   2. scan_loop()：后台线程周期扫描本地消息表，补偿两类丢失窗口：
//      - PENDING 超过 5 秒：事务二提交后、发布/confirm 前进程崩溃
//      - PUBLISHED 且 next_retry_at 到期：消费端失败登记的有界重试
//   发布端单连接非线程安全：内部互斥锁保护（confirm 等待最多 3s，锁粒度
//   以消息为单位，导入 QPS 有限时可接受；更高吞吐需连接池，见演进点）
// ============================================================================
#pragma once

#include <mutex>
#include <string>

#include "common/Config.h"
#include "db/MySQLPool.h"
#include "mq/BackupPublisher.h"

namespace pacs {
namespace mq {

class BackupDispatcher {
public:
    BackupDispatcher(const common::Config &cfg, db::MySQLPool &pool);

    // 发布消息并把 backup_event 置为 PUBLISHED；失败留在 PENDING 由扫描补偿
    void dispatch(const std::string &task_id, uint64_t instance_id);

    // 后台线程入口：循环扫描补偿（内部自行处理 publisher 重连）
    void scan_loop();

    void stop() { stopped_ = true; }

private:
    bool ensure_publisher_locked(std::string &err);

    common::Config cfg_;
    db::MySQLPool &pool_;
    BackupPublisher publisher_;
    std::mutex mtx_;     // 保护 publisher_（rabbitmq-c 连接非线程安全）
    bool stopped_;
};

} // namespace mq
} // namespace pacs
