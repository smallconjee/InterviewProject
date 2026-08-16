// ============================================================================
// Logger.cpp — 日志模块实现
// 输出格式：<本地时间.毫秒> [<级别>] [PACS Archive] [<文件名>:<行号>] <正文>
// 输出目标：stdout（容器场景由 docker logs 收集；文件滚动是已知演进点）
// ============================================================================
#include "common/Logger.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <sys/time.h>

namespace pacs {
namespace common {

// 原子全局级别：读写均无锁，级别切换对业务线程无干扰
static std::atomic<int> g_log_level(LOG_LEVEL_INFO);

// 输出流：默认 stdout；CLI 工具切 stderr（原子指针读写，切换无需停服）
static std::atomic<std::FILE *> g_log_output(NULL);

void set_log_output(std::FILE *f) {
    g_log_output.store(f, std::memory_order_relaxed);
}

static std::FILE *log_output() {
    std::FILE *f = g_log_output.load(std::memory_order_relaxed);
    return f != NULL ? f : stdout;
}

LogLevel get_log_level() {
    return static_cast<LogLevel>(g_log_level.load(std::memory_order_relaxed));
}

void set_log_level(LogLevel level) {
    g_log_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

static const char *level_name(LogLevel level) {
    // 级别名定宽输出（%-5s），保证不同级别的日志列对齐，便于肉眼扫描
    switch (level) {
        case LOG_LEVEL_TRACE: return "TRACE";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default:              return "UNKWN";
    }
}

LogMessage::LogMessage(const char *file, int line, LogLevel level)
    : level_(level) {
    struct timeval tv;
    ::gettimeofday(&tv, NULL);
    struct tm tm_buf;
    // localtime_r 是线程安全版本；localtime 内部用静态缓冲区，多线程会互相覆盖
    ::localtime_r(&tv.tv_sec, &tm_buf);

    // 只取文件名，去掉路径，避免日志行过长
    const char *base = file;
    for (const char *p = file; *p != '\0'; ++p) {
        if (*p == '/') {
            base = p + 1;
        }
    }

    char prefix[160];
    ::snprintf(prefix, sizeof(prefix),
               "%04d-%02d-%02d %02d:%02d:%02d.%03d [%-5s] [PACS Archive] [%s:%d] ",
               tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
               tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
               static_cast<int>(tv.tv_usec / 1000),
               level_name(level_), base, line);
    oss_ << prefix;
}

LogMessage::~LogMessage() {
    std::string line = oss_.str();
    line.push_back('\n');

    // 整行一次 fwrite：POSIX stdio 对 FILE* 的单次调用内部持锁，
    // 加上"一行只调用一次"，可保证与其他线程的日志行不交错
    std::FILE *out = log_output();
    ::fwrite(line.data(), 1, line.size(), out);

    // 重定向到文件时是全缓冲，WARN 以上立即刷新，
    // 避免进程崩溃丢失最后的关键日志
    if (level_ >= LOG_LEVEL_WARN) {
        ::fflush(out);
    }
}

} // namespace common
} // namespace pacs
