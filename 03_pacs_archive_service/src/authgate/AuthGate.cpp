#include "auth/AuthGate.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include <workflow/WFFacilities.h>
#include <ppconsul/ppconsul.h>
#include <ppconsul/health.h> // 元头文件不含 health，需显式引入

#include "auth.srpc.h"

#include "common/Logger.h"

namespace pacs {
namespace auth {

// Consul catalog 查询服务实例地址；失败返回 false（调用方回落默认地址）
static bool consul_lookup(const std::string &consul_host, int consul_port,
                          const std::string &service, std::string &host, int &port) {
    try {
        ppconsul::Consul consul("http://" + consul_host + ":" + std::to_string(consul_port));
        // health 查询只返回通过健康检查的实例（剔除已下线节点）
        ppconsul::health::Health health(consul);
        auto services = health.service(service);
        if (services.empty()) {
            return false;
        }
        const ppconsul::Node &node = std::get<0>(services.front());
        const ppconsul::ServiceInfo &svc = std::get<1>(services.front());
        // ServiceInfo.address 优先（服务自报地址），为空回落节点地址
        host = svc.address.empty() ? node.address : svc.address;
        port = static_cast<int>(svc.port);
        return !host.empty() && port > 0;
    } catch (const std::exception &e) {
        LOG_WARN << "[AuthGate] Consul 查询异常: " << e.what();
        return false;
    }
}

AuthGate::AuthGate(const common::Config &cfg)
    : cfg_(cfg), port_(cfg.server.auth_port), disabled_(false), resolved_(false) {
    const char *flag = ::getenv("PACS_AUTH_DISABLED");
    disabled_ = (flag != NULL && ::strcmp(flag, "1") == 0);
}

void AuthGate::resolve_addr(bool force) {
    if (resolved_ && !force) return;

    // 1) 环境变量显式指定（部署/调试最直接）
    const char *addr = ::getenv("PACS_AUTH_ADDR");
    if (addr != NULL && ::strchr(addr, ':') != NULL) {
        std::string a(addr);
        host_ = a.substr(0, a.rfind(':'));
        port_ = std::atoi(a.c_str() + a.rfind(':') + 1);
        resolved_ = true;
        LOG_INFO << "[AuthGate] 认证服务地址(环境变量): " << host_ << ":" << port_;
        return;
    }

    // 2) Consul 服务发现（health 查询，剔除不健康实例）
    std::string host;
    int port = 0;
    if (consul_lookup(cfg_.consul.host, cfg_.consul.port, "pacs-auth", host, port)) {
        host_ = host;
        port_ = port;
        resolved_ = true;
        LOG_INFO << "[AuthGate] 认证服务地址(Consul 发现): " << host_ << ":" << port_;
        return;
    }

    // 3) 回落默认（同机部署时的约定地址）
    host_ = "127.0.0.1";
    port_ = cfg_.server.auth_port;
    resolved_ = true;
    LOG_WARN << "[AuthGate] Consul 发现失败，回落默认地址: " << host_ << ":" << port_;
}

void AuthGate::init() {
    if (disabled_) {
        LOG_WARN << "[AuthGate] 鉴权已通过 PACS_AUTH_DISABLED=1 关闭（仅限本地调试）";
        return;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    resolve_addr(false);
}

int AuthGate::authorize(const std::string &token, const std::string &action,
                        std::string &username, std::string &err) {
    if (disabled_ || token.empty()) {
        if (disabled_) return 0;
        err = "缺少 Authorization: Bearer <token>";
        return 401;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    resolve_addr(false);

    // 同步 RPC：生成代码自带 sync 调用形态，wfrest 处理线程内阻塞等待
    // （与同步连接池的取舍一致；QPS 高时演进为异步串联进 SeriesWork）
    ::pacs::auth::AuthService::SRPCClient client(host_.c_str(),
                                                 static_cast<unsigned short>(port_));

    ::pacs::auth::VerifyRequest vreq;
    vreq.set_token(token);
    vreq.set_action(action);
    ::pacs::auth::VerifyResponse vresp;
    srpc::RPCSyncContext sync_ctx;
    client.Verify(&vreq, &vresp, &sync_ctx);

    int code;
    std::string message;
    if (!sync_ctx.success) {
        code = 503;
        message = "认证服务调用失败: " + sync_ctx.errmsg;
    } else if (vresp.code() == 0) {
        code = 0;
        message = vresp.message();
        username = vresp.username();
    } else if (vresp.code() == 2) {
        code = 403;
        message = vresp.message();
        username = vresp.username();
    } else {
        code = 401;
        message = vresp.message();
    }

    if (code != 0) {
        err = message;
        if (code == 503) {
            resolved_ = false; // 服务可能漂移，下次重查 Consul
        }
    }
    return code;
}

} // namespace auth
} // namespace pacs
