# 使用说明（usage_guide）

> 日常操作手册：环境怎么起、服务怎么跑、学习怎么练、坏了怎么修。
> 面试当天也用这份：演示部分照"第六节"的逐幕注释口播即可。

---

## 一、每日启动清单（2 分钟）

1. **Mac**：确认 OrbStack 在运行（菜单栏图标）
2. **VS Code** 打开本目录 → `Cmd+Shift+P` → `Dev Containers: Reopen in Container`
3. 容器终端里确认 5 个容器都活着：
   ```bash
   docker ps --format '{{.Names}} {{.Status}}'   # dev/mysql/redis/rabbitmq/consul 五个 Up
   ```
4. 完成。中间件随 compose 自动启动；二进制已编译在 `build/bin/`。

> 首次环境才需要（现在都不用再做）：`install_deps.sh` → `init_db.sh` →
> `seed_users.sh` → 宿主机 `gen_dicom.py` / `gen_reports.py`。

---

## 二、服务启停

**启动顺序：先认证，再主服务**（RIS 独立，无依赖）。

```bash
cd /workspace    # 容器内工作目录

# ① 认证微服务（srpc :8100，注册 Consul）
nohup ./build/bin/pacs_auth_service > /tmp/auth.log 2>&1 &

# ② PACS 主服务（HTTP :8080；PACS_AUTH_DISABLED=1 可跳过鉴权调试）
nohup ./build/bin/pacs_archive_service > /tmp/p.log 2>&1 &

# ③ 备份消费者（演示 OSS 时才需要，凭据从环境变量注入）
PACS_OSS_ENDPOINT=oss-cn-wuhan-lr.aliyuncs.com \
PACS_OSS_AK=<新AK> PACS_OSS_SK=<新SK> PACS_OSS_BUCKET=clouddisk-wuhan-a1b2c3d4 \
nohup ./build/bin/pacs_backup_consumer > /tmp/consumer.log 2>&1 &

# ④ RIS 检索服务（TCP :9090；索引已构建在 data/ris/index）
RIS_INDEX_DIR=data/ris/index nohup stdbuf -oL ./build/bin/ris_report_search > /tmp/ris.log 2>&1 &
```

**看日志**：`tail -f /tmp/p.log`（auth/consumer/ris 同理）。
**停止**：`pkill -f "bin/pacs_archive_service$"`（按名字锚定，见第七节的坑）。
**消费者特殊**：阻塞消费设计下 SIGTERM 会直接退出（未 ACK 消息由 broker 重投，安全）。

---

## 三、常用命令速查

### PACS（容器内执行）

```bash
B=./build/bin/pacs_archive_service
curl -s localhost:8080/db/ping                        # 连接池健康检查
$B --login doctor doctor123                           # 登录拿 token（stdout 纯 token）
$B --parse testdata/dicom/01_ok_explicit.dcm          # DICOM 解析工具
$B --sha256 testdata/dicom/01_ok_explicit.dcm         # 哈希工具

TOKEN=$($B --login doctor doctor123)
curl -H "Authorization: Bearer $TOKEN" localhost:8080/api/v1/studies?patient_id=P001
curl --data-binary @testdata/dicom/01_ok_explicit.dcm \
     -H "Authorization: Bearer $TOKEN" \
     localhost:8080/api/v1/images/import              # 201 新/200 重复/409 冲突

# 中间件直查
docker exec medical-pacs-dev-mysql-1 mysql -uroot -proot pacs_db \
  -e "SELECT id,status,backup_status FROM sop_instance"
docker exec medical-pacs-dev-rabbitmq-1 rabbitmqctl list_queues name messages | grep pacs
docker exec medical-pacs-dev-redis-1 redis-cli --scan --pattern 'ris:v*'
```

### RIS（Mac 宿主机执行）

```bash
# 测试客户端（注意：必须用系统 python，anaconda 被本机防火墙拦截！）
RIS_HOST=medical-pacs-dev-dev-1.orb.local /usr/bin/python3 scripts/test_ris_client.py
```

### 重建/更新

```bash
cmake --build build                     # 新增 .cpp 后要先 cmake -B build -G Ninja 重新 GLOB
RIS_INDEX_DIR=data/ris/index ./build/bin/ris_report_search --build-index testdata/ris/reports
```

---

## 四、Mac ↔ 容器网络（必看两个坑）

| 要访问 | 地址 |
|---|---|
| 中间件（MySQL/Redis/RabbitMQ 台/Consul） | `localhost:<compose映射端口>` |
| **服务进程**（8080/8100/9090，未映射端口） | `medical-pacs-dev-dev-1.orb.local:<端口>` |

- **坑 1**：OrbStack 未映射端口的容器走 `*.orb.local` 域名，且只有 IPv4 可达——
  python 里用 `getaddrinfo(AF_INET)` 强制 v4（`test_ris_client.py` 已内置）。
- **坑 2**：anaconda python 会被 macOS 防火墙拦（Connection refused/No route），
  `/usr/bin/python3` 正常。遇到"连不上"先换系统 python 再排查别的。

---

## 五、一档重写工作流（核心学习循环）

工作区已 `git init` 并打了**基线提交**（全部参考实现）。每个模块的循环：

```bash
# ① 读考题：只看需求，不看参考
open 05_docs/tier1_rewrite_specs.md     # 对应模块小节

# ② 闭卷重写：直接改参考文件的位置（git 会保住原版）
#    例：TLV 解码器 → 04_ris_report_search/src/protocol/TlvCodec.cpp 清空重写

# ③ 编译 + 自测（验收标准就在 spec 里）
cmake --build build --target ris_report_search
RIS_HOST=... /usr/bin/python3 scripts/test_ris_client.py

# ④ 对答案：看差异 → 差异点回弹药库对应章节
git diff                                # 你的版本 vs 基线
git diff --stat                         # 先看改动范围

# ⑤ 两种结束方式
git checkout -- <文件>                  # 恢复参考实现（推荐，保持代码库"正确版本"）
# 或 git add + git commit               # 认为自己的写法更好时保留
```

> 建议节奏：一个模块一口气写完再 diff，中途别偷看——diff 时看到自己漏了边界
> 处理（半帧/UTF-8/双重归还）才最有记忆点，正好对应弹药库的踩坑实录。

---

## 六、演示脚本逐幕说明（面试口播用）

### demo_pacs.sh（容器内跑，~1 分钟）

| 幕 | 输出 | 口播要点 |
|---|---|---|
| 2 | token | srpc 独立认证服务，PBKDF2 校验后签发 JWT |
| 3-4 | 401/403 | 网关中间件→Verify RPC→RBAC 矩阵（radiologist 无 backup:manage） |
| 5 | 201 | SeriesWork 链：落盘→解析→流式哈希→两事务；**本地消息表与归档同事务** |
| 6 | 200 | 同 UID 同哈希幂等返回（唯一索引裁决，不是先查后插） |
| 7 | 409 | 同 UID 异哈希拒绝，不静默覆盖（医疗数据不可变） |
| 9 | PUBLISHED | publisher confirm：broker 确认落盘才标记 |
| 10 | 队列深度 | 消息持久化堆积，等消费者处理（异步链解耦请求线程） |

### demo_ris.sh（Mac 跑，~1 分钟）

| 幕 | 输出 | 口播要点 |
|---|---|---|
| 2 | 6 项测试 | TLV 增量解码：半帧留缓冲、多帧循环取、非法帧断连 |
| 3 | has_image | 报告含 StudyInstanceUID 且 pacs_db 有此 Study → 关联影像 |
| 4 | v1/v2 键 | 索引版本号进缓存键：切换后新键生效，旧键 TTL 回收 |

---

## 七、故障排查表

| 症状 | 原因 | 处理 |
|---|---|---|
| `Address already in use` | 端口被旧进程占 | `pkill -f "bin/<名字>$"`；不行就 `pgrep -f` 找 pid 后 `kill -9` |
| 消费者 `403 ACCESS_REFUSED ... exclusive use` | 有僵尸消费者占着队列（阻塞消费无法响应 TERM） | `pgrep -f backup_consumer$` → `kill -9` |
| 改了检索代码"没生效" | **Redis/LRU 里存着旧结果** | `docker exec medical-pacs-dev-redis-1 redis-cli FLUSHALL` 后重启服务 |
| 服务连不上 MySQL | MySQL 容器重启过 | 连接池借出前 ping+重连自动恢复，等下一请求即可（可演示） |
| 容器重建后编译报错找不到库 | 新容器没装依赖 | `bash scripts/install_deps.sh`（幂等，已装的跳过） |
| muduo 编译 static_assert 失败 | ARM64 long double 128 位 | 安装脚本已含 sed 修复，重跑安装脚本 |
| Mac python 连不上服务 | anaconda 被防火墙拦 / 用了 localhost | 用 `/usr/bin/python3` + `*.orb.local` |
| 导入 400"请求体为空" | 没带文件或路径错 | `--data-binary @testdata/dicom/xx.dcm`（@ 别丢） |

---

## 八、文档地图

| 文件 | 用途 | 什么时候看 |
|---|---|---|
| `05_docs/usage_guide.md` | 本文件：日常操作 | 遇到操作问题 |
| `05_docs/tier1_rewrite_specs.md` | 一档闭卷考卷（7 模块需求+验收） | 每次重写前 |
| `05_docs/interview_ammo.md` | 弹药库：12 模块讲解+追问卡+踩坑 | diff 后对答案、面试前复习 |
| `05_docs/architecture_overview.md` | 链路全景图 + 实现状态 | 讲架构叙事时 |
| `sql/pacs_db_init.sql` | 表结构（注释即设计理由） | 讲数据模型时 |
| README.md | 快速开始 + 端口表 | 给别人看时 |
