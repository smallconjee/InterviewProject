// ============================================================================
// Config.cpp — 配置模块实现：INI 解析、三级合并、启动期校验
// 容错策略（刻意不对称）：
//   文件不存在 -> 告警继续（默认值兜底，本地开发友好）
//   文件格式错误 / 校验失败 -> 返回 false，调用方应启动即退出（fail-fast）
// ============================================================================
#include "common/Config.h"

#include "common/Logger.h"

#include <cstdlib>
#include <cctype>
#include <climits>
#include <fstream>
#include <map>

namespace pacs {
namespace common {

// ---------- INI 解析（最小可用） ----------

// 去掉首尾空白（空格/Tab/CR/LF）
static std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    // isspace 的参数必须转为 unsigned char：char 为负（如中文首字节）时是未定义行为
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 解析结果放入 "section.key" -> value 的平面 map
static bool parse_ini_file(const std::string &path,
                           std::map<std::string, std::string> &out,
                           std::string &err) {
    std::ifstream in(path.c_str());
    if (!in.is_open()) {
        return true; // 文件不存在：不算错误，由调用方决定是否告警
    }

    std::string line;
    std::string section;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        line = trim(line); // 顺带去掉 Windows 换行的 \r
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line[0] == '[') {
            size_t close = line.find(']');
            if (close == std::string::npos || close == 1) {
                err = "第 " + std::to_string(lineno) + " 行: 节名格式非法";
                return false;
            }
            section = trim(line.substr(1, close - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            err = "第 " + std::to_string(lineno) + " 行: 缺少 key=value 结构";
            return false;
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty()) {
            err = "第 " + std::to_string(lineno) + " 行: key 为空";
            return false;
        }
        out[section + "." + key] = val;
    }
    return true;
}

// ---------- 三级取值：环境变量 > INI > 默认值 ----------

// 单个配置项的三级合并取值；新增配置项只需在 load_config 里加一行 pick/pick_int
static std::string pick(const std::map<std::string, std::string> &kv,
                        const char *env_name, const char *ini_key,
                        const std::string &def) {
    const char *env = ::getenv(env_name);
    if (env != NULL && env[0] != '\0') {
        return std::string(env);
    }
    std::map<std::string, std::string>::const_iterator it = kv.find(ini_key);
    if (it != kv.end()) {
        return it->second;
    }
    return def;
}

// 整型版本：解析失败（非数字/溢出）不致命，回退默认值并告警；范围问题留给后面的校验
static int pick_int(const std::map<std::string, std::string> &kv,
                    const char *env_name, const char *ini_key, int def) {
    std::string s = pick(kv, env_name, ini_key, std::to_string(def));
    char *end = NULL;
    long v = ::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || v < INT_MIN || v > INT_MAX) {
        LOG_WARN << "配置项 " << ini_key << " 的值 \"" << s << "\" 不是合法整数，使用默认值 " << def;
        return def;
    }
    return static_cast<int>(v);
}

// ---------- 加载 + 校验 ----------

bool load_config(Config &cfg, const std::string &path, std::string &err) {
    std::map<std::string, std::string> kv;
    if (!path.empty()) {
        if (!parse_ini_file(path, kv, err)) {
            err = path + " " + err;
            return false;
        }
        if (kv.empty()) {
            LOG_WARN << "配置文件不存在（或为空），跳过文件层: " << path
                     << "，仅使用环境变量 + 默认值";
        }
    }

    cfg.mysql.host     = pick(kv, "PACS_MYSQL_HOST",     "mysql.host",     cfg.mysql.host);
    cfg.mysql.port     = pick_int(kv, "PACS_MYSQL_PORT", "mysql.port",     cfg.mysql.port);
    cfg.mysql.user     = pick(kv, "PACS_MYSQL_USER",     "mysql.user",     cfg.mysql.user);
    cfg.mysql.password = pick(kv, "PACS_MYSQL_PASSWORD", "mysql.password", cfg.mysql.password);
    cfg.mysql.database = pick(kv, "PACS_MYSQL_DATABASE", "mysql.database", cfg.mysql.database);
    cfg.mysql.pool_size = pick_int(kv, "PACS_MYSQL_POOL_SIZE", "mysql.pool_size", cfg.mysql.pool_size);

    cfg.rabbitmq.host     = pick(kv, "PACS_RABBITMQ_HOST",     "rabbitmq.host",     cfg.rabbitmq.host);
    cfg.rabbitmq.port     = pick_int(kv, "PACS_RABBITMQ_PORT", "rabbitmq.port",     cfg.rabbitmq.port);
    cfg.rabbitmq.user     = pick(kv, "PACS_RABBITMQ_USER",     "rabbitmq.user",     cfg.rabbitmq.user);
    cfg.rabbitmq.password = pick(kv, "PACS_RABBITMQ_PASSWORD", "rabbitmq.password", cfg.rabbitmq.password);

    cfg.oss.endpoint           = pick(kv, "PACS_OSS_ENDPOINT", "oss.endpoint", cfg.oss.endpoint);
    cfg.oss.access_key_id      = pick(kv, "PACS_OSS_AK",       "oss.ak",       cfg.oss.access_key_id);
    cfg.oss.access_key_secret  = pick(kv, "PACS_OSS_SK",       "oss.sk",       cfg.oss.access_key_secret);
    cfg.oss.bucket             = pick(kv, "PACS_OSS_BUCKET",   "oss.bucket",   cfg.oss.bucket);

    cfg.consul.host = pick(kv, "PACS_CONSUL_HOST",     "consul.host", cfg.consul.host);
    cfg.consul.port = pick_int(kv, "PACS_CONSUL_PORT", "consul.port", cfg.consul.port);

    cfg.server.listen_addr = pick(kv, "PACS_SERVER_LISTEN_ADDR", "server.listen_addr", cfg.server.listen_addr);
    cfg.server.http_port   = pick_int(kv, "PACS_SERVER_HTTP_PORT", "server.http_port",  cfg.server.http_port);
    cfg.server.auth_port   = pick_int(kv, "PACS_SERVER_AUTH_PORT", "server.auth_port",  cfg.server.auth_port);
    cfg.server.temp_dir    = pick(kv, "PACS_SERVER_TEMP_DIR",    "server.temp_dir",    cfg.server.temp_dir);
    cfg.server.storage_dir = pick(kv, "PACS_SERVER_STORAGE_DIR", "server.storage_dir", cfg.server.storage_dir);

    // 校验：启动即失败（fail-fast），一次性报全所有问题
    if (cfg.mysql.host.empty() || cfg.mysql.user.empty() || cfg.mysql.database.empty()) {
        err = "mysql.host/user/database 不能为空";
        return false;
    }
    const int ports[] = {cfg.mysql.port, cfg.rabbitmq.port, cfg.consul.port,
                         cfg.server.http_port, cfg.server.auth_port};
    const char *names[] = {"mysql.port", "rabbitmq.port", "consul.port",
                           "server.http_port", "server.auth_port"};
    for (int i = 0; i < 5; ++i) {
        if (ports[i] <= 0 || ports[i] > 65535) {
            err = std::string(names[i]) + " 超出合法范围(1-65535): " + std::to_string(ports[i]);
            return false;
        }
    }
    if (cfg.mysql.pool_size < 1 || cfg.mysql.pool_size > 64) {
        err = "mysql.pool_size 超出合理范围(1-64): " + std::to_string(cfg.mysql.pool_size);
        return false;
    }
    if (cfg.server.temp_dir.empty() || cfg.server.storage_dir.empty()) {
        err = "server.temp_dir / server.storage_dir 不能为空";
        return false;
    }
    // OSS 阶段 2 才启用：配置了 endpoint 就要求凭据齐全，否则只提示
    if (!cfg.oss.endpoint.empty()) {
        if (cfg.oss.access_key_id.empty() || cfg.oss.access_key_secret.empty() ||
            cfg.oss.bucket.empty()) {
            err = "已配置 oss.endpoint，则 oss.ak/sk/bucket 均不能为空";
            return false;
        }
    } else {
        LOG_WARN << "未配置 OSS（阶段 2 启用异步备份时必须通过环境变量提供 endpoint/ak/sk/bucket）";
    }
    return true;
}

std::string mask(const std::string &secret) {
    return secret.empty() ? std::string("(未配置)") : std::string("***");
}

std::string config_summary(const Config &cfg) {
    std::string s;
    s += "mysql=" + cfg.mysql.user + ":" + mask(cfg.mysql.password) + "@" + cfg.mysql.host +
         ":" + std::to_string(cfg.mysql.port) + "/" + cfg.mysql.database;
    s += " rabbitmq=" + cfg.rabbitmq.user + ":" + mask(cfg.rabbitmq.password) + "@" +
         cfg.rabbitmq.host + ":" + std::to_string(cfg.rabbitmq.port);
    s += " consul=" + cfg.consul.host + ":" + std::to_string(cfg.consul.port);
    s += " http=" + cfg.server.listen_addr + ":" + std::to_string(cfg.server.http_port);
    s += " temp_dir=" + cfg.server.temp_dir;
    if (cfg.oss.endpoint.empty()) {
        s += " oss=(未配置)";
    } else {
        s += " oss=" + cfg.oss.bucket + "@" + cfg.oss.endpoint +
             "(ak=" + mask(cfg.oss.access_key_id) + ",sk=" + mask(cfg.oss.access_key_secret) + ")";
    }
    return s;
}

} // namespace common
} // namespace pacs
