// ============================================================================
// pacs_auth_service — 独立认证微服务（srpc + Protobuf + Consul）
//
// 职责（简历 bullet 5）：
//   Login  : 用户名/密码校验（PBKDF2-HMAC-SHA256 + 随机盐）→ 签发 JWT(HS256)
//   Verify : JWT 验签 + 过期检查 + RBAC 动作鉴权（radiologist/admin）
// 服务注册：Consul（TCP 健康检查）；网关侧通过 Consul 发现本服务
// 密码永不落日志；JWT 密钥走环境变量 PACS_JWT_SECRET（默认值仅供本地开发）
// ============================================================================
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <workflow/WFFacilities.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <ppconsul/ppconsul.h>

#include "auth.srpc.h"

#include "Jwt.h"
#include "common/Config.h"
#include "common/Logger.h"
#include "db/MySQLPool.h"

static WFFacilities::WaitGroup g_wait_group(1);

static void on_signal(int) { g_wait_group.done(); }

namespace pacs {
namespace auth {

namespace {

// 十六进制串 ↔ 字节
bool hex_decode(const std::string &hex, unsigned char *out, size_t expect) {
    if (hex.size() != expect * 2) return false;
    for (size_t i = 0; i < expect; ++i) {
        unsigned v = 0;
        if (std::sscanf(hex.c_str() + i * 2, "%2x", &v) != 1) return false;
        out[i] = static_cast<unsigned char>(v);
    }
    return true;
}

std::string hex_encode(const unsigned char *data, size_t len) {
    static const char *tbl = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = tbl[data[i] >> 4];
        out[i * 2 + 1] = tbl[data[i] & 15];
    }
    return out;
}

// PBKDF2-HMAC-SHA256：迭代 10 万次 + 16 字节随机盐，抵抗彩虹表与暴力枚举
bool pbkdf2_verify(const std::string &password, const std::string &salt_hex,
                   const std::string &expect_hex) {
    unsigned char salt[16];
    unsigned char expect[32];
    unsigned char got[32];
    if (!hex_decode(salt_hex, salt, sizeof(salt))) return false;
    if (!hex_decode(expect_hex, expect, sizeof(expect))) return false;
    if (::PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()), salt,
                            sizeof(salt), 100000, ::EVP_sha256(), sizeof(got), got) != 1) {
        return false;
    }
    return ::CRYPTO_memcmp(expect, got, sizeof(expect)) == 0;
}

// RBAC 矩阵：role → 允许的 action 集合
bool role_allows(const std::string &role, const std::string &action) {
    if (role == "admin") {
        return true; // 管理员全放行（含 backup:manage）
    }
    if (role == "radiologist") {
        return action == "images:import" || action == "studies:read";
    }
    return false; // 未知角色一律拒绝（默认关闭原则）
}

} // namespace

class AuthServiceImpl : public ::pacs::auth::AuthService::Service {
public:
    AuthServiceImpl(db::MySQLPool &pool, const std::string &secret)
        : pool_(pool), secret_(secret) {}

    void Login(::pacs::auth::LoginRequest *req, ::pacs::auth::LoginResponse *resp,
               srpc::RPCContext *ctx) override {
        const std::string &user = req->username();
        const std::string &pass = req->password();
        if (user.empty() || pass.empty()) {
            resp->set_code(1);
            resp->set_message("用户名或密码为空");
            return;
        }

        db::MySQLGuard g = pool_.acquire(3000);
        if (!g) {
            resp->set_code(2);
            resp->set_message("内部错误：数据库不可用");
            return;
        }
        char sql[256];
        std::snprintf(sql, sizeof(sql),
                      "SELECT password_hash, salt, role FROM user_account WHERE username='%s'",
                      user.c_str()); // user 含引号会被拒：额外做一层过滤
        if (user.find('\'') != std::string::npos || user.find('"') != std::string::npos) {
            resp->set_code(1);
            resp->set_message("用户名或密码错误");
            return;
        }
        if (::mysql_query(g.get(), sql) != 0) {
            resp->set_code(2);
            resp->set_message("内部错误：查询失败");
            return;
        }
        MYSQL_RES *rs = ::mysql_store_result(g.get());
        if (rs == NULL || ::mysql_num_rows(rs) == 0) {
            if (rs != NULL) ::mysql_free_result(rs);
            // 统一错误文案：不暴露"用户不存在"（用户枚举防护）
            resp->set_code(1);
            resp->set_message("用户名或密码错误");
            return;
        }
        MYSQL_ROW row = ::mysql_fetch_row(rs);
        std::string hash = row[0] ? row[0] : "";
        std::string salt = row[1] ? row[1] : "";
        std::string role = row[2] ? row[2] : "";
        ::mysql_free_result(rs);

        if (!pbkdf2_verify(pass, salt, hash)) {
            resp->set_code(1);
            resp->set_message("用户名或密码错误");
            LOG_INFO << "[Auth] 登录失败(密码错误): user=" << user;
            return;
        }
        int64_t ttl = 7200; // 2 小时
        resp->set_code(0);
        resp->set_message("登录成功");
        resp->set_token(jwt_sign(user, role, ttl, secret_));
        resp->set_expires_in(ttl);
        LOG_INFO << "[Auth] 登录成功: user=" << user << " role=" << role;
    }

    void Verify(::pacs::auth::VerifyRequest *req, ::pacs::auth::VerifyResponse *resp,
                srpc::RPCContext *ctx) override {
        JwtClaims claims;
        std::string err;
        if (!jwt_verify(req->token(), secret_, claims, err)) {
            resp->set_code(1);
            resp->set_message("token 无效: " + err);
            resp->set_allowed(false);
            return;
        }
        resp->set_username(claims.username);
        resp->set_role(claims.role);
        if (!role_allows(claims.role, req->action())) {
            resp->set_code(2);
            resp->set_message("权限不足: role=" + claims.role + " action=" + req->action());
            resp->set_allowed(false);
            LOG_WARN << "[Auth] 鉴权拒绝: user=" << claims.username
                     << " role=" << claims.role << " action=" << req->action();
            return;
        }
        resp->set_code(0);
        resp->set_message("允许");
        resp->set_allowed(true);
    }

private:
    db::MySQLPool &pool_;
    std::string secret_;
};

} // namespace auth
} // namespace pacs

int main() {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    pacs::common::Config cfg;
    std::string err;
    const char *cfg_path = ::getenv("PACS_CONFIG");
    if (!pacs::common::load_config(cfg, cfg_path ? cfg_path : "", err)) {
        LOG_ERROR << "[Auth] 配置加载失败: " << err;
        return 1;
    }

    pacs::db::MySQLPool pool(cfg.mysql);
    if (!pool.init(err)) {
        LOG_ERROR << "[Auth] MySQL 初始化失败: " << err;
        return 1;
    }

    const char *secret = ::getenv("PACS_JWT_SECRET");
    std::string jwt_secret = (secret != NULL && *secret != '\0')
                                 ? std::string(secret)
                                 : std::string("dev-only-secret-change-me");
    if (jwt_secret == "dev-only-secret-change-me") {
        LOG_WARN << "[Auth] 使用开发默认 JWT 密钥，生产必须设置 PACS_JWT_SECRET";
    }

    const char *advertise = ::getenv("PACS_ADVERTISE_ADDR");
    std::string addr = (advertise != NULL && *advertise != '\0')
                           ? std::string(advertise)
                           : std::string("dev"); // compose 网络内的 dev 容器主机名

    // srpc 服务
    srpc::SRPCServer server;
    pacs::auth::AuthServiceImpl impl(pool, jwt_secret);
    server.add_service(&impl);

    if (server.start(cfg.server.auth_port) != 0) {
        LOG_ERROR << "[Auth] srpc 服务启动失败: 端口 " << cfg.server.auth_port;
        return 1;
    }
    LOG_INFO << "[Auth] srpc 认证服务已启动: " << addr << ":" << cfg.server.auth_port;

    // Consul 注册（TCP 健康检查，由 Consul 主动探测；失败不影响服务本身运行）
    try {
        ppconsul::Consul consul("http://" + cfg.consul.host + ":" +
                                std::to_string(cfg.consul.port));
        ppconsul::agent::Agent agent(consul);
        agent.registerService(
            "pacs-auth",
            ppconsul::agent::kw::address = addr,
            ppconsul::agent::kw::port = static_cast<unsigned short>(cfg.server.auth_port),
            ppconsul::agent::kw::tags = ppconsul::Tags{"auth", "srpc"},
            ppconsul::agent::kw::check = ppconsul::agent::TcpCheck(
                addr, static_cast<unsigned short>(cfg.server.auth_port),
                std::chrono::seconds(10), std::chrono::seconds(3)));
        LOG_INFO << "[Auth] 已注册到 Consul: " << cfg.consul.host << ":" << cfg.consul.port
                 << " service=pacs-auth addr=" << addr << ":" << cfg.server.auth_port;
    } catch (const std::exception &e) {
        LOG_WARN << "[Auth] Consul 注册失败(服务继续运行): " << e.what();
    }

    g_wait_group.wait();
    server.stop();
    LOG_INFO << "[Auth] 服务退出";
    return 0;
}
