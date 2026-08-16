// ============================================================================
// main.cpp — PACS 影像归档与异步备份服务入口（wfrest 路由版）
//
// 启动顺序：信号注册 -> 配置(fail-fast) -> MySQL 连接池预热 -> 路由注册 -> 监听
// 路由：
//   GET  /                        存活检查
//   GET  /db/ping                 连接池健康检查
//   POST /api/v1/images/import    影像导入（multipart 文件域或 octet-stream body）
//   GET  /api/v1/studies          检查列表（?patient_id=&issuer= 过滤）
//   GET  /api/v1/studies/{uid}    检查详情（含序列与实例数）
//   GET  /api/v1/instances/{uid}  实例状态（归档/备份状态机查询）
//   GET  /api/v1/admin/backup-status  备份任务流水（仅 admin 角色）
//   POST /api/v1/auth/login        登录换 JWT（Web 控制台用，srpc 调认证服务）
//   GET  /api/v1/reports/search    RIS 报告检索桥接（TLV 0x01 → RIS TCP 9090）
//   GET  /api/v1/reports/suggest   RIS 拼写纠错桥接（TLV 0x02）
//   GET  /ui/*                     Web 控制台静态资源（06_web/，同源免 CORS）
// 工具模式：
//   --parse <file>   解析并打印 DICOM 元数据
//   --sha256 <file>  计算文件 SHA-256
// ============================================================================
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFGlobal.h>

#include "archive/ArchiveDao.h"
#include "archive/ArchiveService.h"
#include "auth/AuthGate.h"
#include "auth.srpc.h"
#include "common/Config.h"
#include "common/Logger.h"
#include "db/MySQLPool.h"
#include "dicom/DicomReader.h"
#include "mq/BackupDispatcher.h"
#include "risproxy/RisTlvClient.h"
#include "util/Sha256.h"

static WFFacilities::WaitGroup wait_group(1);

void sig_handler(int signo) {
    // 信号处理函数中只能调用 async-signal-safe 函数：stdio/cout 均不安全，用 write(2)
    const char msg[] = "\n[PACS Archive] Receiving shutdown signal...\n";
    ::write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    (void)signo;
    wait_group.done();
}

// ---------- JSON 辅助：手写拼装（不引 JSON 库，转义规则自己控制） ----------

static std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c; // UTF-8 中文原样保留
                }
        }
    }
    return out;
}

// ============================================================================
// 工具模式（不初始化日志/配置/连接池）
// ============================================================================
static int run_parse_tool(const char *path) {
    std::FILE *f = std::fopen(path, "rb");
    if (f == NULL) {
        std::fprintf(stderr, "[PACS Archive] 无法打开文件: %s\n", path);
        return 1;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> buf(static_cast<size_t>(size));
    if (size > 0 && std::fread(buf.data(), 1, static_cast<size_t>(size), f) !=
                        static_cast<size_t>(size)) {
        std::fclose(f);
        std::fprintf(stderr, "[PACS Archive] 读取文件不完整: %s\n", path);
        return 1;
    }
    std::fclose(f);

    pacs::dicom::DicomInfo info;
    std::string err;
    pacs::dicom::ParseResult r =
        pacs::dicom::parse_dicom(buf.data(), buf.size(), info, err);
    if (r != pacs::dicom::PARSE_OK) {
        std::fprintf(stderr, "[PACS Archive] 解析失败 [%s]: %s\n",
                     pacs::dicom::parse_result_name(r), err.c_str());
        return 1;
    }
    std::printf("transfer_syntax  = %s\n", info.transfer_syntax_uid.c_str());
    std::printf("patient          = %s (%s, issuer=%s, %s, %s)\n",
                info.patient_name.c_str(), info.patient_id.c_str(), info.issuer.c_str(),
                info.birth_date.c_str(), info.sex.c_str());
    std::printf("study            = %s date=%s acc=%s\n",
                info.study_instance_uid.c_str(), info.study_date.c_str(),
                info.accession_number.c_str());
    std::printf("series           = %s modality=%s no=%d\n",
                info.series_instance_uid.c_str(), info.modality.c_str(), info.series_number);
    std::printf("sop_instance     = %s (class=%s, no=%d)\n",
                info.sop_instance_uid.c_str(), info.sop_class_uid.c_str(),
                info.instance_number);
    return 0;
}

static int run_sha256_tool(const char *path) {
    pacs::util::FileHash fh = pacs::util::sha256_file(path);
    if (!fh.ok) {
        std::fprintf(stderr, "[PACS Archive] SHA-256 失败: %s\n", fh.err.c_str());
        return 1;
    }
    std::printf("%s  %lld  %s\n", fh.hex.c_str(), static_cast<long long>(fh.size), path);
    return 0;
}

// 登录调试工具：./pacs_archive_service --login <user> <pass>（认证服务需已启动）
static int run_login_tool(const char *user, const char *pass) {
    pacs::common::set_log_output(stderr); // stdout 只输出 token 本身（供 $(...) 捕获）
    pacs::common::Config cfg;
    std::string err;
    if (!pacs::common::load_config(cfg, "", err)) {
        std::fprintf(stderr, "[PACS Archive] 配置加载失败: %s\n", err.c_str());
        return 1;
    }
    ::pacs::auth::AuthService::SRPCClient client("127.0.0.1",
                                                 (unsigned short)cfg.server.auth_port);
    ::pacs::auth::LoginRequest req;
    req.set_username(user);
    req.set_password(pass);
    ::pacs::auth::LoginResponse resp;
    srpc::RPCSyncContext ctx;
    client.Login(&req, &resp, &ctx);
    if (!ctx.success) {
        std::fprintf(stderr, "[PACS Archive] 认证服务调用失败: %s\n", ctx.errmsg.c_str());
        return 1;
    }
    if (resp.code() != 0) {
        std::fprintf(stderr, "[PACS Archive] 登录失败: %s\n", resp.message().c_str());
        return 1;
    }
    std::printf("%s\n", resp.token().c_str());
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc == 3 && ::strcmp(argv[1], "--parse") == 0) {
        return run_parse_tool(argv[2]);
    }
    if (argc == 3 && ::strcmp(argv[1], "--sha256") == 0) {
        return run_sha256_tool(argv[2]);
    }
    if (argc == 4 && ::strcmp(argv[1], "--login") == 0) {
        return run_login_tool(argv[2], argv[3]);
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // 配置三级合并：环境变量 > INI 文件(PACS_CONFIG 指定) > 默认值
    pacs::common::Config cfg;
    std::string cfg_err;
    const char *cfg_path = ::getenv("PACS_CONFIG");
    if (!pacs::common::load_config(cfg, cfg_path ? cfg_path : "", cfg_err)) {
        LOG_ERROR << "配置加载失败: " << cfg_err;
        return 1;
    }
    LOG_INFO << "配置加载完成: " << pacs::common::config_summary(cfg);

    // 数据库连接池：启动预热、fail-fast
    pacs::db::MySQLPool db_pool(cfg.mysql);
    std::string db_err;
    if (!db_pool.init(db_err)) {
        LOG_ERROR << "MySQL 连接池初始化失败: " << db_err;
        return 1;
    }

    pacs::archive::ArchiveService archive_service(cfg, db_pool);
    pacs::archive::ArchiveDao dao(db_pool);
    pacs::mq::BackupDispatcher backup_dispatcher(cfg, db_pool);
    pacs::auth::AuthGate auth_gate(cfg);
    auth_gate.init();
    // 扫描线程在 server.start 成功后才创建：早退路径不存在"未 join 线程"的 terminate 风险
    std::thread scanner_thread;

    LOG_INFO << "==========================================";
    LOG_INFO << " [PACS 影像归档与异步备份系统] 启动中...";
    LOG_INFO << "==========================================";

    wfrest::HttpServer svr;

    // 鉴权中间件语义：通过返回 true；否则已写好 401/403/503 响应
    auto auth_check = [&auth_gate](const wfrest::HttpReq *req, wfrest::HttpResp *resp,
                                   const char *action) -> bool {
        if (auth_gate.disabled()) return true;
        const std::string &ha = req->header("Authorization");
        const std::string prefix = "Bearer ";
        std::string token;
        if (ha.compare(0, prefix.size(), prefix) == 0 && ha.size() > prefix.size()) {
            token = ha.substr(prefix.size());
        }
        std::string username, err;
        int rc = auth_gate.authorize(token, action, username, err);
        if (rc == 0) return true;
        std::string j = "{\"code\":" + std::to_string(rc) + ",\"message\":\"" +
                        json_escape(err) + "\"}";
        resp->String(j);
        resp->set_status(rc);
        return false;
    };

    svr.POST("/api/v1/images/import",
             [&archive_service, &backup_dispatcher, &auth_check](
                 const wfrest::HttpReq *req, wfrest::HttpResp *resp) {
        if (!auth_check(req, resp, "images:import")) return;
        // 优先取 multipart 文件域；否则退回 octet-stream 原始 body
        const char *data = NULL;
        size_t len = 0;
        auto &form = req->form();
        for (auto it = form.begin(); it != form.end(); ++it) {
            if (!it->second.second.empty()) {
                data = it->second.second.data();
                len = it->second.second.size();
                break;
            }
        }
        if (data == NULL) {
            const std::string &body = req->body();
            if (!body.empty()) {
                data = body.data();
                len = body.size();
            }
        }
        if (data == NULL || len == 0) {
            std::string j = "{\"code\":400,\"message\":\"请求体为空，需要 DICOM 文件\"}";
            resp->String(j);
            resp->set_status(400);
            return;
        }
        pacs::archive::ImportResult r = archive_service.import_image(data, len);
        // 归档事务提交成功 → 立刻走快路径发布备份消息（慢路径由扫描线程兜底）
        if (r.http_status == 201 && !r.task_id.empty()) {
            backup_dispatcher.dispatch(r.task_id, r.instance_id);
        }
        char j[512];
        std::snprintf(j, sizeof(j),
                      "{\"code\":%d,\"status\":\"%s\",\"instance_id\":%llu,"
                      "\"task_id\":\"%s\",\"detail\":\"%s\"}",
                      r.http_status == 500 ? 500 : 0, r.status_word.c_str(),
                      (unsigned long long)r.instance_id, r.task_id.c_str(),
                      json_escape(r.detail).c_str());
        resp->String(j);
        resp->set_status(r.http_status);
    });

    svr.GET("/api/v1/studies", [&dao, &auth_check](const wfrest::HttpReq *req,
                                                   wfrest::HttpResp *resp) {
        if (!auth_check(req, resp, "studies:read")) return;
        std::vector<pacs::archive::StudyRow> rows;
        std::string err;
        if (!dao.list_studies(req->query("patient_id"), req->query("issuer"), rows, err)) {
            std::string j = "{\"code\":500,\"message\":\"" + json_escape(err) + "\"}";
            resp->String(j);
            resp->set_status(500);
            return;
        }
        std::string j = "{\"code\":0,\"count\":" + std::to_string(rows.size()) + ",\"studies\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            pacs::archive::StudyRow &s = rows[i];
            char item[768];
            std::snprintf(item, sizeof(item),
                          "%s{\"study_instance_uid\":\"%s\",\"patient_name\":\"%s\","
                          "\"patient_id\":\"%s\",\"issuer\":\"%s\",\"study_date\":\"%s\","
                          "\"accession_number\":\"%s\",\"study_description\":\"%s\","
                          "\"instance_count\":%d}",
                          i == 0 ? "" : ",", json_escape(s.study_instance_uid).c_str(),
                          json_escape(s.patient_name).c_str(), json_escape(s.patient_id).c_str(),
                          json_escape(s.issuer).c_str(), s.study_date.c_str(),
                          json_escape(s.accession_number).c_str(),
                          json_escape(s.study_description).c_str(), s.instance_count);
            j += item;
        }
        j += "]}";
        resp->String(j);
    });

    svr.GET("/api/v1/studies/{uid}", [&dao, &auth_check](const wfrest::HttpReq *req,
                                                         wfrest::HttpResp *resp) {
        if (!auth_check(req, resp, "studies:read")) return;
        std::vector<pacs::archive::SeriesRow> rows;
        std::string err;
        if (!dao.get_series_of_study(req->param("uid"), rows, err)) {
            std::string j = "{\"code\":500,\"message\":\"" + json_escape(err) + "\"}";
            resp->String(j);
            resp->set_status(500);
            return;
        }
        std::string j = "{\"code\":0,\"series\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            pacs::archive::SeriesRow &s = rows[i];
            char item[512];
            std::snprintf(item, sizeof(item),
                          "%s{\"series_instance_uid\":\"%s\",\"modality\":\"%s\","
                          "\"series_number\":%d,\"instance_count\":%d}",
                          i == 0 ? "" : ",", json_escape(s.series_instance_uid).c_str(),
                          json_escape(s.modality).c_str(), s.series_number, s.instance_count);
            j += item;
        }
        j += "]}";
        resp->String(j);
    });

    svr.GET("/api/v1/instances/{uid}", [&dao](const wfrest::HttpReq *req, wfrest::HttpResp *resp) {
        pacs::archive::InstanceRow row;
        bool found = false;
        std::string err;
        if (!dao.get_instance(req->param("uid"), row, found, err)) {
            std::string j = "{\"code\":500,\"message\":\"" + json_escape(err) + "\"}";
            resp->String(j);
            resp->set_status(500);
            return;
        }
        if (!found) {
            resp->String("{\"code\":404,\"message\":\"实例不存在\"}");
            resp->set_status(404);
            return;
        }
        char j[640];
        std::snprintf(j, sizeof(j),
                      "{\"code\":0,\"sop_instance_uid\":\"%s\",\"status\":\"%s\","
                      "\"backup_status\":\"%s\",\"sha256\":\"%s\",\"file_size\":%llu,"
                      "\"storage_path\":\"%s\"}",
                      json_escape(row.sop_instance_uid).c_str(), row.status.c_str(),
                      row.backup_status.c_str(), row.sha256.c_str(),
                      (unsigned long long)row.file_size, json_escape(row.storage_path).c_str());
        resp->String(j);
    });

    svr.GET("/api/v1/admin/backup-status", [&dao, &auth_check](const wfrest::HttpReq *req,
                                                               wfrest::HttpResp *resp) {
        // 管理员专属：本地消息表状态机观测（backup:manage 仅 admin 角色拥有）
        if (!auth_check(req, resp, "backup:manage")) return;
        std::vector<pacs::archive::ArchiveDao::BackupEventRow> rows;
        std::string err;
        if (!dao.list_recent_backup_events(20, rows, err)) {
            std::string j = "{\"code\":500,\"message\":\"" + json_escape(err) + "\"}";
            resp->String(j);
            resp->set_status(500);
            return;
        }
        std::string j = "{\"code\":0,\"events\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            char item[256];
            std::snprintf(item, sizeof(item),
                          "%s{\"task_id\":\"%s\",\"instance_id\":%llu,\"status\":\"%s\","
                          "\"retry_count\":%d}",
                          i == 0 ? "" : ",", json_escape(rows[i].task_id).c_str(),
                          (unsigned long long)rows[i].instance_fk,
                          rows[i].status.c_str(), rows[i].retry_count);
            j += item;
        }
        j += "]}";
        resp->String(j);
    });

    svr.GET("/db/ping", [&db_pool](const wfrest::HttpReq *req, wfrest::HttpResp *resp) {
        pacs::db::MySQLGuard guard = db_pool.acquire(3000);
        if (!guard) {
            resp->String("{\"code\":500,\"message\":\"获取数据库连接超时\"}");
            resp->set_status(500);
            return;
        }
        if (::mysql_query(guard.get(), "SELECT 1") != 0) {
            std::string m = std::string("SQL 执行失败: ") + ::mysql_error(guard.get());
            resp->String("{\"code\":500,\"message\":\"" + json_escape(m) + "\"}");
            resp->set_status(500);
            return;
        }
        MYSQL_RES *res = ::mysql_store_result(guard.get());
        if (res != NULL) {
            ::mysql_free_result(res);
        }
        resp->String("{\"code\":0,\"message\":\"mysql ok\"}");
    });

    svr.GET("/", [](const wfrest::HttpReq *req, wfrest::HttpResp *resp) {
        resp->String("{\"code\":0,\"message\":\"PACS Archive Service is running.\"}");
    });

    // ==================== Web 控制台支撑（纯增量：不影响以上既有路由） ====================

    // RIS 桥接地址：环境变量 PACS_RIS_ADDR（host:port）> 默认 127.0.0.1:9090
    std::string ris_host;
    unsigned short ris_port = 0;
    pacs::risproxy::resolve_ris_addr(ris_host, ris_port);
    LOG_INFO << "[RIS Proxy] RIS 检索服务地址: " << ris_host << ":" << ris_port;

    // 静态资源挂载：Web 控制台在 /ui（与 API 同源，免 CORS）。根目录相对 CWD，
    // 须从仓库根目录启动；wfrest Static 只映射 GET /ui/* 到文件，目录本身不出
    // index.html，故补 /ui 与 /ui/ 两个 302 跳转方便直接输入地址
    {
        std::FILE *f = std::fopen("./06_web/index.html", "rb");
        if (f != NULL) {
            std::fclose(f);
            LOG_INFO << "Web 控制台已就绪: http://localhost:" << cfg.server.http_port
                     << "/ui/ （静态根目录 ./06_web）";
        } else {
            LOG_WARN << "未找到 ./06_web/index.html（需从仓库根目录启动），/ui 将不可用";
        }
    }
    svr.Static("/ui", "./06_web");
    svr.GET("/ui", [](const wfrest::HttpReq *req, wfrest::HttpResp *resp) {
        resp->headers["Location"] = "/ui/index.html";
        resp->set_status(302);
    });
    svr.GET("/ui/", [](const wfrest::HttpReq *req, wfrest::HttpResp *resp) {
        resp->headers["Location"] = "/ui/index.html";
        resp->set_status(302);
    });

    // 登录接口：Web 前端换取 JWT（srpc 同步调用——与 --login 工具、AuthGate::
    // authorize 的现状一致，占用一个 handler 线程，取舍见弹药库讨论）
    svr.POST("/api/v1/auth/login", [&cfg](const wfrest::HttpReq *req, wfrest::HttpResp *resp) {
        // wfrest::Json 类型陷阱防护：parse 非法 JSON 得到 node_=null 的对象，
        // 直接 has()/get 会解引用空指针；非字符串节点取值同样危险——三重校验
        auto json_str = [&req](const char *key, std::string &out) -> bool {
            if (req->body().empty()) return false;
            wfrest::Json j = wfrest::Json::parse(req->body());
            if (!j.is_valid() || !j.has(key) || !j[key].is_string()) return false;
            out = j[key].get<std::string>();
            return true;
        };
        std::string user, pass;
        json_str("username", user);
        json_str("password", pass);
        if (user.empty() || pass.empty()) {
            // 注意：resp->Json 的两个重载对 const char* 有歧义（wfrest::Json 存在
            // 隐式构造），字符串字面量必须显式构造 std::string
            resp->Json(std::string(
                "{\"code\":400,\"message\":\"需要 JSON 字段 username 与 password\"}"));
            resp->set_status(400);
            return;
        }
        // 与 run_login_tool 相同的直连模式：认证服务默认同机 127.0.0.1:auth_port
        // （PACS_AUTH_ADDR 覆盖只作用于 AuthGate 的 Verify，此处不跟随——已知限制）
        ::pacs::auth::AuthService::SRPCClient client(
            "127.0.0.1", (unsigned short)cfg.server.auth_port);
        ::pacs::auth::LoginRequest rpc_req;
        rpc_req.set_username(user);
        rpc_req.set_password(pass);
        ::pacs::auth::LoginResponse rpc_resp;
        srpc::RPCSyncContext ctx;
        client.Login(&rpc_req, &rpc_resp, &ctx);
        if (!ctx.success) {
            std::string j = "{\"code\":503,\"message\":\"认证服务调用失败: " +
                            json_escape(ctx.errmsg) + "\"}";
            resp->Json(j);
            resp->set_status(503);
            return;
        }
        if (rpc_resp.code() != 0) {
            std::string j = "{\"code\":401,\"message\":\"" +
                            json_escape(rpc_resp.message()) + "\"}";
            resp->Json(j);
            resp->set_status(401);
            return;
        }
        // 响应不含 role：LoginResponse 协议里没有该字段，前端从 JWT payload 解码
        char j[512];
        std::snprintf(j, sizeof(j),
                      "{\"code\":0,\"token\":\"%s\",\"username\":\"%s\",\"expires_in\":%lld}",
                      json_escape(rpc_resp.token()).c_str(), json_escape(user).c_str(),
                      (long long)rpc_resp.expires_in());
        resp->Json(std::string(j));
    });

    // RIS 桥接：reports/search(TLV 0x01) 与 reports/suggest(TLV 0x02)。
    // handler 第三参拿到 SeriesWork，把 TLV 客户端任务串进当前 HTTP 任务的
    // series——回调里写响应，全程异步不占 handler 线程（workflow 核心用法）
    auto add_ris_route = [&svr, &auth_check, ris_host, ris_port](
                             const char *path, uint8_t frame_type) {
        svr.GET(path, [&auth_check, ris_host, ris_port, frame_type](
                          const wfrest::HttpReq *req, wfrest::HttpResp *resp,
                          SeriesWork *series) {
            if (!auth_check(req, resp, "studies:read")) return;
            // wfrest 的 query() 不做百分号解码：浏览器 encodeURIComponent 后的
            // 中文查询串必须手动还原，否则 RIS 拿到的是 %XX 字面量（实测踩坑）
            std::string raw = req->query("query");
            std::string q;
            q.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); i++) {
                if (raw[i] == '%' && i + 2 < raw.size() &&
                    ::isxdigit(static_cast<unsigned char>(raw[i + 1])) &&
                    ::isxdigit(static_cast<unsigned char>(raw[i + 2]))) {
                    auto hex = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    int hi = hex(raw[i + 1]), lo = hex(raw[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        q += static_cast<char>(hi * 16 + lo);
                        i += 2;
                        continue;
                    }
                }
                q += (raw[i] == '+') ? ' ' : raw[i];
            }
            if (q.empty()) {
                resp->Json(std::string("{\"code\":400,\"message\":\"缺少 query 参数\"}"));
                resp->set_status(400);
                return;
            }
            pacs::risproxy::RisTlvTask *task = pacs::risproxy::create_ris_task(
                ris_host, ris_port, frame_type, q,
                [resp](pacs::risproxy::RisTlvTask *t) {
                    if (t->get_state() != WFT_STATE_SUCCESS) {
                        std::string j =
                            std::string("{\"code\":502,\"message\":\"RIS 检索服务不可达: ") +
                            json_escape(WFGlobal::get_error_string(t->get_state(),
                                                                   t->get_error())) +
                            "\"}";
                        resp->set_status(502);
                        resp->Json(j);
                        return;
                    }
                    const pacs::risproxy::RisTlvMessage *m = t->get_resp();
                    if (m->type() == pacs::risproxy::RIS_TYPE_ERROR) {
                        std::string j = "{\"code\":502,\"message\":\"RIS 返回错误帧: " +
                                        json_escape(m->value()) + "\"}";
                        resp->set_status(502);
                        resp->Json(j);
                        return;
                    }
                    // RIS 响应帧 payload 即 JSON 文本，原样透传（Json() 自带合法性校验）
                    resp->Json(m->value());
                });
            series->push_back(task);
        });
    };
    add_ris_route("/api/v1/reports/search", pacs::risproxy::RIS_TYPE_SEARCH_REQ);
    add_ris_route("/api/v1/reports/suggest", pacs::risproxy::RIS_TYPE_SUGGEST_REQ);

    if (svr.start(cfg.server.http_port) == 0) {
        scanner_thread = std::thread(&pacs::mq::BackupDispatcher::scan_loop, &backup_dispatcher);
        LOG_INFO << "wfrest HTTP 服务已在端口 " << cfg.server.http_port << " 成功启动。";
        LOG_INFO << "可在 Mac 浏览器访问: http://localhost:" << cfg.server.http_port;
        wait_group.wait();
        svr.stop();
    } else {
        LOG_ERROR << "服务启动失败！";
        return 1;
    }

    backup_dispatcher.stop();
    if (scanner_thread.joinable()) {
        scanner_thread.join();
    }
    LOG_INFO << "服务已平稳退出。";
    return 0;
}
