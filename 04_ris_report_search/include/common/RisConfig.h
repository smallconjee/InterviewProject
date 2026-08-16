// ============================================================================
// RisConfig.h — RIS 服务配置（纯环境变量 + 默认值）
//
// 为什么不做 INI/文件层：RIS 的配置项全部由部署环境决定（compose 注入），
// 没有团队共享非敏感配置的需求——YAGNI；与 PACS 的三级加载形成取舍对比
// （面试讲点：配置复杂度应该匹配实际需求，不是越通用越好）。
// ============================================================================
#pragma once

#include <cstdlib>
#include <string>

namespace ris {
namespace common {

struct RisConfig {
    int listen_port = 9090;              // env RIS_LISTEN_PORT
    int io_threads = 4;                  // env RIS_IO_THREADS（muduo sub-loop 数，默认 4）
    std::string dict_dir = "/usr/local/dict";   // env RIS_DICT_DIR（cppjieba 词典）
    std::string index_dir = "data/ris/index";   // env RIS_INDEX_DIR（版本目录父目录）
    std::string mysql_host = "mysql";    // env RIS_MYSQL_HOST（pacs_db 关联查询）
    int mysql_port = 3306;               // env RIS_MYSQL_PORT
    std::string mysql_user = "root";     // env RIS_MYSQL_USER
    std::string mysql_password = "root"; // env RIS_MYSQL_PASSWORD
    std::string redis_host = "redis";    // env RIS_REDIS_HOST
    int redis_port = 6379;               // env RIS_REDIS_PORT
    size_t lru_capacity = 512;           // env RIS_LRU_CAPACITY
    int cache_ttl = 300;                 // env RIS_CACHE_TTL（秒）

    static RisConfig from_env() {
        RisConfig c;
        const char *s;
        if ((s = ::getenv("RIS_LISTEN_PORT")) != NULL) c.listen_port = std::atoi(s);
        if ((s = ::getenv("RIS_IO_THREADS")) != NULL) c.io_threads = std::atoi(s);
        if ((s = ::getenv("RIS_DICT_DIR")) != NULL && *s) c.dict_dir = s;
        if ((s = ::getenv("RIS_INDEX_DIR")) != NULL && *s) c.index_dir = s;
        if ((s = ::getenv("RIS_MYSQL_HOST")) != NULL && *s) c.mysql_host = s;
        if ((s = ::getenv("RIS_MYSQL_PORT")) != NULL) c.mysql_port = std::atoi(s);
        if ((s = ::getenv("RIS_MYSQL_USER")) != NULL && *s) c.mysql_user = s;
        if ((s = ::getenv("RIS_MYSQL_PASSWORD")) != NULL) c.mysql_password = s;
        if ((s = ::getenv("RIS_REDIS_HOST")) != NULL && *s) c.redis_host = s;
        if ((s = ::getenv("RIS_REDIS_PORT")) != NULL) c.redis_port = std::atoi(s);
        if ((s = ::getenv("RIS_LRU_CAPACITY")) != NULL) c.lru_capacity = std::atoi(s);
        if ((s = ::getenv("RIS_CACHE_TTL")) != NULL) c.cache_ttl = std::atoi(s);
        return c;
    }
};

} // namespace common
} // namespace ris
