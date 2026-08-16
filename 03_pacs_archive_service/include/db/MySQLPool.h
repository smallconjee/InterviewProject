// ============================================================================
// MySQLPool.h — 固定大小同步 MySQL 连接池
//
// 生命周期：main 启动时构造 + init() 预热（fail-fast），进程退出时析构回收。
// 并发模型：mutex + condition_variable 保护空闲队列；ping/重连刻意放在锁外，
//           避免数据库抖动时长时间持锁拖住所有等连接的线程。
// 断连策略：借出前 mysql_ping 校验，失效则显式重连（不用 AUTO_RECONNECT，
//           它会在事务中途静默换连接、丢会话状态，详见 05_docs 弹药库第 4 节）。
// ============================================================================
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

#include <mysql/mysql.h>

#include "common/Config.h"

namespace pacs {
namespace db {

class MySQLPool;

// RAII 连接守卫：构造时从池获得连接，析构时自动归还。
// 只可移动不可拷贝——归还权唯一，防止同一连接被归还两次。
class MySQLGuard {
public:
    MySQLGuard(MySQLPool *pool, MYSQL *conn) : pool_(pool), conn_(conn) {}

    MySQLGuard(const MySQLGuard &) = delete;
    MySQLGuard &operator=(const MySQLGuard &) = delete;

    MySQLGuard(MySQLGuard &&other) : pool_(other.pool_), conn_(other.conn_) {
        other.pool_ = NULL;
        other.conn_ = NULL;
    }

    MySQLGuard &operator=(MySQLGuard &&other) {
        if (this != &other) {
            reset();
            pool_ = other.pool_;
            conn_ = other.conn_;
            other.pool_ = NULL;
            other.conn_ = NULL;
        }
        return *this;
    }

    ~MySQLGuard() { reset(); }

    MYSQL *get() { return conn_; }

    // 是否持有有效连接（acquire 超时/重连失败时为 false）
    operator bool() const { return conn_ != NULL; }

    // 立即归还连接（提前结束使用时手动调用；析构会自动调用）
    void reset();

private:
    friend class MySQLPool;

    MySQLPool *pool_;
    MYSQL *conn_;
};

// 固定大小同步连接池：
//  - 启动时预热 pool_size 条连接（fail-fast，连不上直接退出）
//  - acquire 超时返回空 Guard，不无限阻塞
//  - 借出前 mysql_ping 校验，失效则显式重连（不用 AUTO_RECONNECT，理由见实现）
// 线程安全：所有成员函数可被多线程并发调用
class MySQLPool {
public:
    explicit MySQLPool(const common::MysqlConfig &cfg);
    ~MySQLPool();

    MySQLPool(const MySQLPool &) = delete;
    MySQLPool &operator=(const MySQLPool &) = delete;

    // 预热建立全部连接；失败时 err 带原因
    bool init(std::string &err);

    // 阻塞获取一条连接（最多 timeout_ms），超时返回空 Guard
    MySQLGuard acquire(int timeout_ms = 3000);

    // 当前空闲连接数（观测/日志用）
    size_t available();

private:
    friend class MySQLGuard;
    MYSQL *create_conn(std::string &err);
    bool ensure_alive(MYSQL *conn);
    void release(MYSQL *conn);

    common::MysqlConfig cfg_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<MYSQL *> idle_; // 空闲连接队列
};

} // namespace db
} // namespace pacs
