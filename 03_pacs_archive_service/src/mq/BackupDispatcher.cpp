#include "mq/BackupDispatcher.h"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#include "common/Logger.h"

namespace pacs {
namespace mq {

namespace {

std::string esc(MYSQL *c, const std::string &s) {
    std::vector<char> buf(2 * s.size() + 1);
    unsigned long n = ::mysql_real_escape_string_quote(c, buf.data(), s.c_str(), s.size(), '\'');
    return std::string(buf.data(), n);
}

} // namespace

BackupDispatcher::BackupDispatcher(const common::Config &cfg, db::MySQLPool &pool)
    : cfg_(cfg), pool_(pool), stopped_(false) {}

bool BackupDispatcher::ensure_publisher_locked(std::string &err) {
    // 拓扑声明是幂等的（durable 声明重复执行无害），重复 init 安全
    return publisher_.init(cfg_.rabbitmq, err);
}

void BackupDispatcher::dispatch(const std::string &task_id, uint64_t instance_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string err;
    if (!ensure_publisher_locked(err)) {
        LOG_WARN << "[备份分发] publisher 不可用(消息暂留 PENDING 待补偿): " << err;
        return;
    }
    if (!publisher_.publish(task_id, instance_id, err)) {
        LOG_WARN << "[备份分发] 发布失败(消息暂留 PENDING 待补偿): " << err;
        return;
    }
    db::MySQLGuard g = pool_.acquire();
    if (!g) {
        LOG_WARN << "[备份分发] 确认后取不到 DB 连接，事件保持 PENDING";
        return;
    }
    std::string sql = "UPDATE backup_event SET status='PUBLISHED' WHERE task_id='" +
                      esc(g.get(), task_id) + "' AND status='PENDING'";
    if (::mysql_query(g.get(), sql.c_str()) != 0) {
        LOG_WARN << "[备份分发] 标记 PUBLISHED 失败: " << ::mysql_error(g.get());
    }
}

void BackupDispatcher::scan_loop() {
    LOG_INFO << "[备份补偿] 扫描线程启动（每 3 秒）";
    while (!stopped_) {
        std::this_thread::sleep_for(std::chrono::seconds(3));

        db::MySQLGuard g = pool_.acquire(2000);
        if (!g) {
            continue;
        }
        // PENDING 超 5s（发布窗口丢失）或 PUBLISHED 到期重试（消费端失败登记）
        std::string sql =
            "SELECT task_id, instance_fk FROM backup_event "
            "WHERE (status='PENDING' AND created_at < NOW(3) - INTERVAL 5 SECOND) "
            "   OR (status='PUBLISHED' AND next_retry_at IS NOT NULL "
            "       AND next_retry_at <= NOW(3) AND retry_count < 5) LIMIT 32";
        if (::mysql_query(g.get(), sql.c_str()) != 0) {
            LOG_WARN << "[备份补偿] 扫描查询失败: " << ::mysql_error(g.get());
            continue;
        }
        MYSQL_RES *rs = ::mysql_store_result(g.get());
        if (rs == NULL) {
            continue;
        }
        struct Item {
            std::string task_id;
            uint64_t instance;
        };
        std::vector<Item> items;
        MYSQL_ROW row;
        while ((row = ::mysql_fetch_row(rs)) != NULL) {
            Item it;
            it.task_id = row[0] ? row[0] : "";
            it.instance = row[1] ? std::strtoull(row[1], NULL, 10) : 0;
            items.push_back(it);
        }
        ::mysql_free_result(rs);
        g.reset(); // 先归还连接：发布与重标记再用新连接，避免长时间占用

        for (size_t i = 0; i < items.size(); ++i) {
            if (stopped_) break;
            std::lock_guard<std::mutex> lk(mtx_);
            std::string err;
            if (!ensure_publisher_locked(err) ||
                !publisher_.publish(items[i].task_id, items[i].instance, err)) {
                LOG_WARN << "[备份补偿] 发布失败 task=" << items[i].task_id << ": " << err;
                continue;
            }
            db::MySQLGuard g2 = pool_.acquire(2000);
            if (!g2) continue;
            std::string upd = "UPDATE backup_event SET status='PUBLISHED', next_retry_at=NULL "
                              "WHERE task_id='" + esc(g2.get(), items[i].task_id) + "'";
            if (::mysql_query(g2.get(), upd.c_str()) != 0) {
                LOG_WARN << "[备份补偿] 更新失败: " << ::mysql_error(g2.get());
            } else {
                LOG_INFO << "[备份补偿] 已补发 task=" << items[i].task_id;
            }
        }
    }
    LOG_INFO << "[备份补偿] 扫描线程退出";
}

} // namespace mq
} // namespace pacs
