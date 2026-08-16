// ============================================================================
// Config.h — 配置模块
//
// 加载策略（三级合并，优先级从高到低）：
//   1. 环境变量（PACS_ 前缀，容器编排注入配置/密钥的标准通道）
//   2. INI 文件（PACS_CONFIG 环境变量指定路径；团队共享的非敏感配置）
//   3. 结构体成员默认值（保证本地开发零配置可跑通）
//
// 安全红线：OSS 凭据只走环境变量；日志输出敏感字段必须过 mask() 打码。
// 设计原则：普通结构体 + 显式传引用，不做单例（依赖可见、可测试）。
// ============================================================================
#pragma once

#include <string>

namespace pacs {
namespace common {

struct MysqlConfig {
    std::string host = "mysql";
    int port = 3306;
    std::string user = "root";
    std::string password = "root";
    std::string database = "pacs_db";
    int pool_size = 4; // 连接池大小：不是越大越好，见 MySQLPool 讲解
};

struct RabbitMqConfig {
    std::string host = "rabbitmq";
    int port = 5672;
    std::string user = "guest";
    std::string password = "guest";
};

struct OssConfig {
    // OSS 凭据只从环境变量注入，绝不写进代码库和配置示例
    std::string endpoint;
    std::string access_key_id;
    std::string access_key_secret;
    std::string bucket;
};

struct ConsulConfig {
    std::string host = "consul";
    int port = 8500;
};

struct ServerConfig {
    std::string listen_addr = "0.0.0.0";
    int http_port = 8080;
    int auth_port = 8100; // 认证微服务（srpc）监听端口
    std::string temp_dir = "/tmp/pacs_tmp"; // 兼容保留：非归档用途临时目录
    // 归档存储根目录（staging 与最终对象同在此目录下，保证 rename 原子、不跨文件系统）
    std::string storage_dir = "data/storage";
};

struct Config {
    MysqlConfig mysql;
    RabbitMqConfig rabbitmq;
    OssConfig oss;
    ConsulConfig consul;
    ServerConfig server;
};

// 三级合并加载：环境变量 > INI 文件 > 结构体默认值
//   path 为空表示跳过文件层（只用环境变量+默认值）
//   文件不存在：告警并继续（默认值足以本地跑通）
//   文件存在但格式错误 / 合并后校验失败：err 带原因，返回 false，调用方应启动即退出
bool load_config(Config &cfg, const std::string &path, std::string &err);

// 敏感字段打码（密码/密钥日志里只显示 ***）
// 敏感字段打码：非空显示 ***，空显示 (未配置)；用于日志输出，杜绝凭据落日志
std::string mask(const std::string &secret);

// 生成单行配置摘要（敏感字段已打码），供启动日志确认生效配置
std::string config_summary(const Config &cfg);

} // namespace common
} // namespace pacs
