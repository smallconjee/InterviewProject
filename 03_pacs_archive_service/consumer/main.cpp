// ============================================================================
// pacs_backup_consumer — 备份消费者（独立进程）
//
// 独立成进程的理由：可单独重启/升级而不影响导入服务；演示"杀掉消费者→
// 消息在队列堆积→重启后不丢不重"最直观；生产上消费端可按积压独立扩容。
//
// 消费语义（at-least-once + 幂等收敛到 exactly-once 效果）：
//   1. 手动 ACK：只有走完决策才 ack
//   2. 毒消息（JSON 解析失败/字段缺失）→ nack(requeue=false) → 死信队列
//   3. task_id 已 CONFIRMED → 重复投递，直接 ack 跳过
//   4. OSS 上传（同 key 同内容覆盖，天然幂等）→ 成功后同一事务：
//      instance.backup_status=BACKED_UP + backup_event.status=CONFIRMED
//   5. 失败 → 有界重试登记（retry_count+1, 指数退避 next_retry_at），ack 等主服务
//      补偿扫描线程补发；retry_count 达 5 次置 DEAD + backup_status=FAILED（终态）
// ============================================================================
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

#include <SimpleAmqpClient/SimpleAmqpClient.h>
#include <nlohmann/json.hpp>

#include "common/Config.h"
#include "common/Logger.h"
#include "db/MySQLPool.h"
#include "oss/OssBackup.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) {
    // SimpleAmqpClient 2.5.1 的阻塞消费无超时、无法被打断：主线程卡在
    // BasicConsumeMessage 里，软退出标志永远等不到检查。这里直接 _exit
    // （async-signal-safe）。安全性由协议保证：未 ACK 的消息会被 broker
    // 重投（at-least-once），消费端幂等吸收重复——幂等设计反过来简化了关闭语义
    const char msg[] = "[备份消费者] 收到退出信号，立即退出（未 ACK 消息将由 broker 重投）\n";
    (void)::write(2, msg, sizeof(msg) - 1);
    ::_exit(0);
}

namespace {

std::string esc(MYSQL *c, const std::string &s) {
    std::vector<char> buf(2 * s.size() + 1);
    unsigned long n = ::mysql_real_escape_string_quote(c, buf.data(), s.c_str(), s.size(), '\'');
    return std::string(buf.data(), n);
}

bool exec(MYSQL *c, const std::string &sql, std::string &err) {
    if (::mysql_query(c, sql.c_str()) != 0) {
        err = ::mysql_error(c);
        return false;
    }
    return true;
}

// 查询备份事件当前状态；found=false 表示事件不存在（异常场景）
bool event_status(MYSQL *c, const std::string &task_id, std::string &status,
                  int &retry_count, bool &found) {
    std::string sql = "SELECT status, retry_count FROM backup_event WHERE task_id='" +
                      esc(c, task_id) + "'";
    if (::mysql_query(c, sql.c_str()) != 0) return false;
    MYSQL_RES *rs = ::mysql_store_result(c);
    if (rs == NULL) return false;
    found = (::mysql_num_rows(rs) > 0);
    if (found) {
        MYSQL_ROW r = ::mysql_fetch_row(rs);
        status = r[0] ? r[0] : "";
        retry_count = r[1] ? std::atoi(r[1]) : 0;
    }
    ::mysql_free_result(rs);
    return true;
}

} // namespace

int main() {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    pacs::common::Config cfg;
    std::string err;
    const char *cfg_path = ::getenv("PACS_CONFIG");
    if (!pacs::common::load_config(cfg, cfg_path ? cfg_path : "", err)) {
        LOG_ERROR << "[备份消费者] 配置加载失败: " << err;
        return 1;
    }

    pacs::db::MySQLPool pool(cfg.mysql);
    if (!pool.init(err)) {
        LOG_ERROR << "[备份消费者] MySQL 初始化失败: " << err;
        return 1;
    }

    pacs::oss::OssBackup oss;
    if (!oss.init(err)) {
        LOG_ERROR << "[备份消费者] " << err;
        return 1;
    }

    LOG_INFO << "[备份消费者] 启动，等待消息: queue=pacs.backup.queue";
    int64_t processed = 0, duplicated = 0, retried = 0, dead = 0;

    while (!g_stop) {
        // 消费者连接：循环内重建，RabbitMQ 重启后自动恢复
        // SimpleAmqpClient 2.5.1 的 BasicConsumeMessage 是阻塞式（无超时重载），
        // 退出依赖当前消息处理完后的下一轮 g_stop 检查（演示场景可接受）
        AmqpClient::Channel::ptr_t channel;
        try {
            channel = AmqpClient::Channel::Create(cfg.rabbitmq.host, cfg.rabbitmq.port,
                                                  cfg.rabbitmq.user, cfg.rabbitmq.password);
        } catch (const std::exception &e) {
            LOG_WARN << "[备份消费者] 连接 RabbitMQ 失败(3s 后重试): " << e.what();
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        // 手动 ACK 模式：no_ack=false；no_local/exclusive 按默认
        std::string tag = channel->BasicConsume("pacs.backup.queue", "pacs-backup-consumer",
                                                true, false);

        try {
            while (!g_stop) {
                AmqpClient::Envelope::ptr_t env = channel->BasicConsumeMessage(tag);
                const std::string &body = env->Message()->Body();

                // ---- 毒消息判定：解析失败进死信队列 ----
                std::string task_id;
                uint64_t instance_id = 0;
                try {
                    nlohmann::json j = nlohmann::json::parse(body);
                    task_id = j.at("task_id").get<std::string>();
                    instance_id = j.at("instance_id").get<uint64_t>();
                } catch (const std::exception &) {
                    LOG_ERROR << "[备份消费者] 毒消息(无法解析)，转死信队列: " << body;
                    // 2.5.1 没有 BasicNack：basic.reject(requeue=false) 同样走路由到 DLX
                    channel->BasicReject(env, false);
                    ++dead;
                    continue;
                }

                pacs::db::MySQLGuard g = pool.acquire(3000);
                if (!g) {
                    LOG_WARN << "[备份消费者] 取不到 DB 连接，nack 重回队列";
                    channel->BasicReject(env, true);
                    continue;
                }

                // ---- 幂等：重复投递直接跳过 ----
                std::string status;
                int retry_count = 0;
                bool found = false;
                if (!event_status(g.get(), task_id, status, retry_count, found)) {
                    channel->BasicReject(env, true);
                    continue;
                }
                if (!found || status == "CONFIRMED" || status == "DEAD") {
                    channel->BasicAck(env);
                    ++duplicated;
                    continue;
                }

                // ---- 取实例的本地路径并上传 OSS ----
                std::string sql = "SELECT storage_path FROM sop_instance WHERE id=" +
                                  std::to_string(instance_id);
                std::string local_rel, oss_err;
                bool has_path = false;
                if (::mysql_query(g.get(), sql.c_str()) == 0) {
                    MYSQL_RES *rs = ::mysql_store_result(g.get());
                    if (rs != NULL) {
                        MYSQL_ROW r = ::mysql_fetch_row(rs);
                        if (r != NULL && r[0] != NULL && r[0][0] != '\0') {
                            local_rel = r[0];
                            has_path = true;
                        }
                        ::mysql_free_result(rs);
                    }
                }

                bool uploaded = false;
                if (has_path) {
                    uploaded = oss.put_object(cfg.server.storage_dir + "/" + local_rel,
                                              local_rel, oss_err);
                } else {
                    oss_err = "实例缺少 storage_path";
                }

                if (uploaded) {
                    // 成功：同一事务更新两个状态，再手动 ACK
                    if (!::mysql_autocommit(g.get(), 0)) {
                        // ignore
                    }
                    std::string u1 = "UPDATE sop_instance SET backup_status='BACKED_UP' WHERE id=" +
                                     std::to_string(instance_id);
                    std::string u2 = "UPDATE backup_event SET status='CONFIRMED' WHERE task_id='" +
                                     esc(g.get(), task_id) + "'";
                    std::string e2;
                    if (exec(g.get(), u1, e2) && exec(g.get(), u2, e2) &&
                        ::mysql_commit(g.get()) == 0) {
                        channel->BasicAck(env);
                        ++processed;
                        LOG_INFO << "[备份消费者] 完成 task=" << task_id << " instance="
                                 << instance_id << " → OSS " << local_rel;
                    } else {
                        ::mysql_rollback(g.get());
                        ::mysql_autocommit(g.get(), 1);
                        LOG_WARN << "[备份消费者] 状态更新失败，nack 重回队列: " << e2;
                        channel->BasicReject(env, true);
                    }
                    ::mysql_autocommit(g.get(), 1);
                } else {
                    // 失败：有界重试登记（主服务扫描线程按 next_retry_at 补发）
                    if (retry_count >= 5) {
                        std::string e3;
                        std::string u1 = "UPDATE backup_event SET status='DEAD' WHERE task_id='" +
                                         esc(g.get(), task_id) + "'";
                        std::string u2 =
                            "UPDATE sop_instance SET backup_status='FAILED' WHERE id=" +
                            std::to_string(instance_id);
                        if (exec(g.get(), u1, e3) && exec(g.get(), u2, e3)) {
                            channel->BasicAck(env); // 终态：不再重回队列
                            LOG_ERROR << "[备份消费者] 重试超限置 DEAD: task=" << task_id
                                      << " 最后错误: " << oss_err;
                        } else {
                            channel->BasicReject(env, true);
                        }
                    } else {
                        // 指数退避：2^(retry+1) 秒
                        std::string u = "UPDATE backup_event SET retry_count=retry_count+1, "
                                        "next_retry_at=NOW(3)+INTERVAL POW(2,retry_count+1) SECOND "
                                        "WHERE task_id='" + esc(g.get(), task_id) + "'";
                        std::string e4;
                        if (exec(g.get(), u, e4)) {
                            channel->BasicAck(env);
                            ++retried;
                            LOG_WARN << "[备份消费者] 上传失败登记重试(第 " << retry_count + 1
                                     << " 次): " << oss_err;
                        } else {
                            channel->BasicReject(env, true);
                        }
                    }
                }
            }
        } catch (const std::exception &e) {
            // 多为消费超时(1s)之外的通道异常：外层重建连接
            LOG_WARN << "[备份消费者] 通道异常，将重建连接: " << e.what();
        }
    }

    LOG_INFO << "[备份消费者] 退出。统计: 完成=" << processed << " 重复=" << duplicated
             << " 重试登记=" << retried << " 死信=" << dead;
    return 0;
}
