# 医学影像系统 (Medical PACS/RIS Workspace)

基于 **macOS + OrbStack + VS Code Dev Containers + Docker Compose** 的 Linux C++ 开发工作区。
C++11，两个独立子系统，全部编译/运行/验证在容器内完成。

## 子系统与二进制（build/bin/）

| 二进制 | 说明 | 端口 |
|---|---|---|
| `pacs_archive_service` | 影像归档与异步备份（wfrest + MySQL + RabbitMQ + OSS + srpc 网关） | HTTP 8080 |
| `pacs_auth_service` | 认证微服务（srpc + PBKDF2 + JWT + RBAC + Consul） | srpc 8100 |
| `pacs_backup_consumer` | 备份消费者（RabbitMQ 手动 ACK + OSS + 有界重试 + DLQ） | — |
| `ris_report_search` | 报告检索（muduo + TinyXML2 + cppjieba + TF-IDF + LRU/Redis） | TCP 9090 (TLV) |

## 快速开始

```bash
# 0) Mac 上运行 OrbStack → VS Code 打开本目录 → Dev Containers: Reopen in Container
# 1) 首次：安装全部依赖（workflow/wfrest/muduo/rabbitmq-c/SimpleAmqpClient/OSS/ppconsul/srpc/tinyxml2/cppjieba/utfcpp/nlohmann）
bash scripts/install_deps.sh
# 2) 建库 + 种子用户（admin/admin123、doctor/doctor123）
bash scripts/init_db.sh && bash scripts/seed_users.sh
# 3) 生成测试数据（宿主机跑）
python3 scripts/gen_dicom.py && python3 scripts/gen_reports.py
# 4) 构建
cmake -B build -G Ninja && cmake --build build
```

## 演示

```bash
# PACS 全链路（登录→401/403→导入201/重复200/冲突409→查询→消息表→队列）—— 容器内
bash scripts/demo_pacs.sh
# RIS 全链路（TLV 6 项测试→纠错→has_image 联动→版本切换）—— Mac 宿主机
bash scripts/demo_ris.sh
```

### 手动联调

```bash
# 容器内：登录拿 token（srpc 调认证服务）
./build/bin/pacs_archive_service --login doctor doctor123
# 导入影像（multipart 或 octet-stream）
curl --data-binary @testdata/dicom/01_ok_explicit.dcm \
     -H "Authorization: Bearer <token>" \
     http://127.0.0.1:8080/api/v1/images/import
# RIS：离线建索引 + 启动检索服务（TLV 测试客户端见 scripts/test_ris_client.py）
./build/bin/ris_report_search --build-index testdata/ris/reports
./build/bin/ris_report_search
```

### OSS 备份消费端（需要真实阿里云凭据）

```bash
export PACS_OSS_ENDPOINT=oss-cn-xxx.aliyuncs.com
export PACS_OSS_AK=<你的AK>  PACS_OSS_SK=<你的SK>  PACS_OSS_BUCKET=<bucket>
./build/bin/pacs_backup_consumer   # 消费 pacs.backup.queue → OSS → BACKED_UP
```

## 关键文档

- `05_docs/architecture_overview.md` — 架构总览（链路图 + 当前实现状态）
- `05_docs/interview_ammo.md` — 面试弹药库（12 模块：设计讲解 + 追问卡 + 踩坑实录）
- `05_docs/usage_guide.md` — **使用说明**（日常启停/命令速查/重写工作流/故障排查）
- `05_docs/tier1_rewrite_specs.md` — 一档模块闭卷重写需求文档（学习路径）

## 中间件（Mac 宿主机访问）

| 服务 | 容器内 | Mac 访问 | 凭据 |
|---|---|---|---|
| MySQL 8.0 | mysql:3306 / pacs_db | 127.0.0.1:3306 | root/root |
| Redis 7 | redis:6379 | 127.0.0.1:6379 | 无 |
| RabbitMQ | rabbitmq:5672 | 5672；管理台 localhost:15672 | guest/guest |
| Consul | consul:8500 | localhost:8500 | 无 |

容器内服务端口（8080/8100/9090）从 Mac 用 OrbStack 域名访问：
`medical-pacs-dev-dev-1.orb.local:<port>`。
