// ============================================================================
// Logger.h — PACS 服务轻量日志模块（自研，不用 spdlog 的取舍见 05_docs 弹药库）
//
// 设计要点：
//   1. 一条日志 = 一个临时 LogMessage 对象，析构时整行一次 fwrite，
//      保证多线程下行与行不交错（行原子性）
//   2. 级别过滤发生在宏入口：被过滤的日志不构造 stringstream，近零开销
//   3. 全局级别用 std::atomic 无锁读写，运行期可调
// 接口约定：LOG_XX 宏必须独占一条语句（"if (...) 临时对象" 形式有悬挂 else 风险）
// ============================================================================
#pragma once

#include <cstdio>
#include <sstream>

namespace pacs {
namespace common {

// 日志级别：数值越大越严重；全局级别默认 INFO
enum LogLevel {
    LOG_LEVEL_TRACE = 0,
    LOG_LEVEL_DEBUG = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_WARN  = 3,
    LOG_LEVEL_ERROR = 4,
};

// 一条日志 = 一个临时 LogMessage 对象：
//   构造时生成时间戳/级别/文件行号前缀，业务代码向 stream() 流式写入正文，
//   析构时把整行（含换行）一次性 fwrite 输出——多线程下保证行与行不交错。
class LogMessage {
public:
    LogMessage(const char *file, int line, LogLevel level);
    ~LogMessage();

    std::ostream &stream() { return oss_; }

private:
    std::ostringstream oss_;
    LogLevel level_;
};

// 获取/设置全局日志级别（atomic 无锁；默认 INFO，见 Logger.cpp）
LogLevel get_log_level();
void set_log_level(LogLevel level);

// 日志输出流（默认 stdout；CLI 工具模式切到 stderr，保证 stdout 只有机读结果）
void set_log_output(std::FILE *f);

} // namespace common
} // namespace pacs

// 级别过滤发生在宏入口：低于全局级别的日志不会构造 stringstream，
// 被过滤的日志几乎零开销。
// 说明：采用 muduo 同款 "if (...) 临时对象" 形式，存在悬挂 else 风险，
//       因此约定 LOG_XX 宏必须独占一条语句使用。
#define PACS_LOG_LEVEL_CHECK(level) \
    if ((level) < ::pacs::common::get_log_level()) { \
    } else \
        ::pacs::common::LogMessage(__FILE__, __LINE__, (level)).stream()

#define LOG_TRACE PACS_LOG_LEVEL_CHECK(::pacs::common::LOG_LEVEL_TRACE)
#define LOG_DEBUG PACS_LOG_LEVEL_CHECK(::pacs::common::LOG_LEVEL_DEBUG)
#define LOG_INFO  PACS_LOG_LEVEL_CHECK(::pacs::common::LOG_LEVEL_INFO)
#define LOG_WARN  PACS_LOG_LEVEL_CHECK(::pacs::common::LOG_LEVEL_WARN)
#define LOG_ERROR PACS_LOG_LEVEL_CHECK(::pacs::common::LOG_LEVEL_ERROR)
