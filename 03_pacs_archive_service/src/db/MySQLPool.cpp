// ============================================================================
// MySQLPool.cpp — 连接池实现
// 已知限制（诚实记录，演进点见弹药库）：
//   若连接失效且补建也失败，该连接从池中永久流失（acquire 超时日志可见征兆）；
//   彻底解法是后台保活线程 + 池低水位补建，当前规模不值得引入复杂度。
// ============================================================================
#include "db/MySQLPool.h"

#include <chrono>

#include "common/Logger.h"

namespace pacs {
namespace db {

MySQLPool::MySQLPool(const common::MysqlConfig &cfg) : cfg_(cfg) {}

MySQLPool::~MySQLPool() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = 0; i < idle_.size(); ++i) {
        ::mysql_close(idle_[i]);
    }
    idle_.clear();
    LOG_INFO << "MySQL 连接池已销毁，剩余连接全部关闭";
}

MYSQL *MySQLPool::create_conn(std::string &err) {
    MYSQL *conn = ::mysql_init(NULL);
    if (conn == NULL) {
        err = "mysql_init 失败（内存不足）";
        return NULL;
    }

    // 不开自动重连（MySQL 8.0 默认就是关，无需显式设置——8.0.34 起
    // MYSQL_OPT_RECONNECT 已废弃并刷告警）：AUTO_RECONNECT 会在连接断开时
    // 静默重建，可能丢事务上下文；改为借出前显式 ping + 重连

    // 连接超时：数据库不可达时避免启动/重连长时间挂死
    unsigned int connect_timeout = 5;
    ::mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);

    if (::mysql_real_connect(conn, cfg_.host.c_str(), cfg_.user.c_str(),
                             cfg_.password.c_str(), cfg_.database.c_str(),
                             cfg_.port, NULL, 0) == NULL) {
        err = std::string("连接失败: ") + ::mysql_error(conn) +
              " (errno=" + std::to_string(::mysql_errno(conn)) + ")";
        ::mysql_close(conn);
        return NULL;
    }

    // MySQL 的 utf8 是残缺的 3 字节实现，患者姓名等中文场景必须 utf8mb4
    if (::mysql_set_character_set(conn, "utf8mb4") != 0) {
        err = std::string("设置 utf8mb4 字符集失败: ") + ::mysql_error(conn);
        ::mysql_close(conn);
        return NULL;
    }
    return conn;
}

bool MySQLPool::ensure_alive(MYSQL *conn) {
    if (::mysql_ping(conn) == 0) {
        return true;
    }
    LOG_WARN << "MySQL 连接已失效(" << ::mysql_error(conn) << ")，尝试重连...";
    // 服务端断开后可在同一句柄上重新 real_connect
    if (::mysql_real_connect(conn, cfg_.host.c_str(), cfg_.user.c_str(),
                             cfg_.password.c_str(), cfg_.database.c_str(),
                             cfg_.port, NULL, 0) != NULL) {
        ::mysql_set_character_set(conn, "utf8mb4");
        return true;
    }
    LOG_ERROR << "MySQL 重连失败: " << ::mysql_error(conn);
    ::mysql_close(conn);
    return false;
}

bool MySQLPool::init(std::string &err) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (int i = 0; i < cfg_.pool_size; ++i) {
        MYSQL *conn = create_conn(err);
        if (conn == NULL) {
            err = "预热第 " + std::to_string(i + 1) + " 条连接时" + err;
            return false; // fail-fast：启动阶段数据库不可用就直接退出
        }
        idle_.push_back(conn);
    }
    LOG_INFO << "MySQL 连接池初始化完成: " << cfg_.pool_size << " 条连接 → "
             << cfg_.user << "@" << cfg_.host << ":" << cfg_.port << "/" << cfg_.database;
    return true;
}

MySQLGuard MySQLPool::acquire(int timeout_ms) {
    // 谓词式 wait_for：被 notify 或超时唤醒后重新检查空闲队列，防虚假唤醒
    std::unique_lock<std::mutex> lock(mtx_);
    bool ok = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this] { return !idle_.empty(); });
    if (!ok) {
        LOG_ERROR << "获取 MySQL 连接超时(" << timeout_ms << "ms)，当前空闲连接数 "
                  << idle_.size() << "/" << cfg_.pool_size;
        return MySQLGuard(this, NULL);
    }
    MYSQL *conn = idle_.front();
    idle_.pop_front();
    lock.unlock();

    // ping/重连放在锁外执行，避免数据库抖动时长时间持锁拖住所有线程
    if (!ensure_alive(conn)) {
        std::string err;
        conn = create_conn(err); // 补一条新连接，维持池大小不变
        if (conn == NULL) {
            LOG_ERROR << "重建连接失败: " << err;
            return MySQLGuard(this, NULL);
        }
    }
    return MySQLGuard(this, conn);
}

void MySQLPool::release(MYSQL *conn) {
    {
        // 先入队再唤醒：保证被唤醒的线程一定能拿到连接
        std::lock_guard<std::mutex> lk(mtx_);
        idle_.push_back(conn);
    }
    cv_.notify_one(); // 每次只归还一条连接，只唤醒一个等待者即可
}

size_t MySQLPool::available() {
    std::lock_guard<std::mutex> lk(mtx_);
    return idle_.size();
}

void MySQLGuard::reset() {
    // 双重保险：pool_ 与 conn_ 任意为空都不动作；移动后源对象两者均为空
    if (pool_ != NULL && conn_ != NULL) {
        pool_->release(conn_);
    }
    pool_ = NULL;
    conn_ = NULL;
}

} // namespace db
} // namespace pacs
