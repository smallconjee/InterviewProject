// ============================================================================
// AuthGate.h — 网关侧鉴权门（wfrest 中间件形态）
//
// 流程：Authorization: Bearer <token> → srpc 同步调用认证服务 Verify →
//       0 放行 / 401 token 无效 / 403 权限不足 / 503 认证服务不可用
// 服务发现：优先环境变量 PACS_AUTH_ADDR（host:port），否则查 Consul catalog，
//       均失败则回落默认地址 auth 端口；发现结果缓存，失败时下次重查
// 演示开关：PACS_AUTH_DISABLED=1 跳过鉴权（仅本地调试用）
// ============================================================================
#pragma once

#include <mutex>
#include <string>

#include "common/Config.h"

namespace pacs {
namespace auth {

class AuthGate {
public:
    explicit AuthGate(const common::Config &cfg);

    // 初始化服务地址（env > Consul > 默认）；不阻塞启动失败
    void init();

    // 返回 HTTP 状态码语义：0=放行，401/403/503=拒绝（err 带原因）
    int authorize(const std::string &token, const std::string &action,
                  std::string &username, std::string &err);

    bool disabled() const { return disabled_; }

private:
    void resolve_addr(bool force);

    common::Config cfg_;
    std::string host_;
    int port_;
    bool disabled_;
    bool resolved_;
    std::mutex mtx_; // srpc 调用串行化（同步 RPC + 客户端非线程安全）
};

} // namespace auth
} // namespace pacs
