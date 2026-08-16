# 面试弹药库（interview_ammo.md）

> 随项目开发逐步积累：每个模块的**设计讲解 + 追问卡 + 踩坑实录**。
> 维护约定：每完成一个开发小步，必须把该步的弹药追加到本文件。
>
> 用法：面试前按模块通读；每条设计取舍要能脱稿讲 1–2 分钟；
> 标记【坑】的是真实踩过的坑，最有说服力，优先讲。

---

## 目录

- [1. 构建系统与依赖管理](#1-构建系统与依赖管理)（0.1 步）
- [2. 日志系统](#2-日志系统)（0.2 步：PACS 自写 Logger + RIS muduo 日志）
- [3. 配置模块](#3-配置模块)（0.3 步：三级合并加载）
- [4. MySQL 连接池](#4-mysql-连接池)（0.4 步：RAII 守卫 + 断连自愈）
- [5. 四层影像数据模型](#5-四层影像数据模型)（阶段 1：pacs_db 建模）
- [6. 手写 DICOM 解析器](#6-手写-dicom-解析器)（阶段 1：DicomReader）
- [7. 导入主链路：事务与幂等决策树](#7-导入主链路事务与幂等决策树阶段-1)（阶段 1）
- [8. 异步备份链：本地消息表 + RabbitMQ 可靠投递](#8-异步备份链本地消息表--rabbitmq-可靠投递阶段-2)（阶段 2）
- [9. 认证微服务：srpc + Consul + JWT + RBAC](#9-认证微服务srpc--consul--jwt--rbac阶段-3)（阶段 3）
- [10. RIS 离线：解析、分词、TF-IDF 倒排索引](#10-ris-离线解析分词tf-idf-倒排索引阶段-4)（阶段 4）
- [11. RIS 在线：TLV 协议、muduo 集成、检索执行](#11-ris-在线tlv-协议muduo-集成检索执行阶段-5)（阶段 5）
- [12. RIS 缓存：LRU + Redis 版本化键](#12-ris-缓存lru--redis-版本化键阶段-6)（阶段 6）

---

## 1. 构建系统与依赖管理

### 设计讲解

**产物统一到 `build/bin/`**：根 CMakeLists 设置 `CMAKE_RUNTIME_OUTPUT_DIRECTORY`，多子项目二进制汇到一个入口——调试配置、演示脚本、CI 都不用猜"这个 target 的产物在哪个子目录"。代价是失去按子目录隔离产物的能力，两服务规模下统一更划算。

**`file(GLOB_RECURSE)` 收集源码的利弊**：方便（新文件自动纳入），但 CMake 在 configure 时生成文件清单，**新增/删除 `.cpp` 后必须重新 `cmake -B build`** 才会被感知。`CONFIGURE_DEPENDS` 可缓解但有每次构建重新扫描的代价。大项目规范做法是 `target_sources` 显式列文件。

**静态链接 workflow/muduo**：安装产出 `.a` 静态库。理由：部署单二进制、无运行时 so 版本漂移；代价是产物大、库升级需全量重编。

**依赖安装脚本策略**：优先从 `00_third_party/` 本地源码拷到 `/tmp` 编译（免联网、不污染存档目录），本地缺失才回退 GitHub（ghfast.to 镜像优先）。

### 追问卡

1. **Q: GLOB_RECURSE 有什么坑，大项目怎么做？**
   A: 新文件不感知需重新 configure；`CONFIGURE_DEPENDS` 缓解但有扫描开销；规范做法 `target_sources` 显式列文件。

2. **Q: `CMAKE_CXX_STANDARD 11` + `STANDARD_REQUIRED ON` 什么意思？**
   A: 要求编译器真正用 `-std=c++11`，禁止静默回退到默认标准；不加 REQUIRED 时老编译器可能悄悄用 c++98 编到一半才报错。

3. **Q: `-std=c++11` 和 `-std=gnu++11` 区别？**
   A: 后者启用 GNU 扩展（当前未设 `CXX_EXTENSIONS OFF`，默认 gnu++11）。跨平台严谨项目会关掉扩展，避免无意用了不可移植特性。

4. **Q: out-of-source build 是什么，为什么？**
   A: 构建产物不混入源码树（`build/` 可整目录删除重来），多构建类型互不污染，gitignore 干净。

5. **Q: 讲一个你解决过的编译期问题。**
   A:（真实经历）ARM64 容器编 muduo 失败：`LogStream.cc` 的 `static_assert(kMaxNumericSize - 10 > std::numeric_limits<long double>::digits10)` 不成立——x86-64 的 long double 是 80 位（digits10=18），而 **aarch64 的 long double 是 128 位（digits10≈33）**，默认缓冲区 32 不够。读报错 → 定位到平台 ABI 差异 → 最小补丁（`kMaxNumericSize` 32→48，sed 进安装脚本）。展示"读编译错误→查根因→最小修复"的路径。

### 踩坑实录

- 【坑】根 `CMakeLists.txt` 的 `add_subdirectory()` 引用了不带数字前缀的旧目录名，根目录 configure 直接失败——目录改名后构建配置没有同步，修复时选择改 CMake 指向 `03_/04_`（保留数字前缀约定）而不是改目录名。
- 【坑】`.vscode/launch.json` 引用了 `preLaunchTask: "CMake: build"` 但没有对应 tasks.json，F5 直接报"任务不存在"——调试配置也是构建链路的一部分，要一起验收。
- 【观察】PACS 服务一次启动绑定 8080 失败，怀疑是上一轮测试连接的 TIME_WAIT 残留 + 监听 socket 的 `SO_REUSEADDR` 行为，60 秒后自愈。可作为"端口占用排查思路"（`ss -tlnp`）的小素材。

---

## 2. 日志系统

### 方案取舍（总述，先背这段）

两服务两种日志方案，是**跟随网络库生态**的刻意设计：
- **PACS（workflow）自写轻量 Logger**：workflow 不带日志模块；需求只有"级别过滤 + 毫秒时间戳 + 前缀 + 线程安全"这 60 分功能，约 150 行可控可讲；生产上会换 spdlog。
- **RIS（muduo）用库自带 Logger**：muduo 的 `AsyncLogging` 双缓冲是《Linux 多线程服务端编程》招牌内容，用 muduo 却不用它的日志，面试官必问。

被问"你们项目日志怎么做的"，答三层：**自己设计了什么、库提供了什么、生产上会换成什么**——比单一答案厚。

### 设计讲解（PACS 自写 Logger）

1. **一条日志 = 一个临时 `LogMessage` 对象，析构时整行一次 `fwrite`**——行原子性的来源。多线程下如果时间戳写一次、正文写一次，行就会交错；整行拼好单次写出，配合 POSIX stdio 对 `FILE*` 单次调用内部持锁，行不撕裂。
2. **级别过滤发生在宏入口，不在输出时**：`LOG_DEBUG` 在全局级别为 INFO 时根本不构造 `ostringstream`，被过滤的日志零格式化开销。写法借 muduo 的 `if (...) 临时对象` 形式（有悬挂 else 风险，约定宏独占一条语句）。
3. **`localtime_r` 而不是 `localtime`**：后者返回静态缓冲区指针，多线程互相覆盖。
4. **全局级别用 `std::atomic<int>`**：读写无锁，运行期可调级别。
5. **WARN 以上立即 `fflush`**：stdout 重定向到文件时是全缓冲，避免崩溃丢最后的关键日志。

### 追问卡

1. **Q: 多线程日志怎么保证不交错？**
   A: 整行一次 `fwrite`（stdio 单次调用内部有锁）。对比错误做法：分次写、每线程 printf 片段。

2. **Q: `fwrite` 到底是不是线程安全的？**
   A: POSIX 要求 stdio 函数线程安全（内部 `flockfile`），但保证的是"单次调用不撕裂"，不保证逻辑上"一行完整"——所以仍要整行一次写。

3. **Q: kill 进程后日志文件为什么可能是空的？**（真实踩坑）
   A: muduo `defaultOutput` 只 `fwrite` 不 flush；stdout 重定向到文件时全缓冲；SIGTERM 非正常终止不 flush → 缓冲区丢失。旧代码没这问题是因为 `std::endl` 每行隐式 flush。解法：`fflush` 策略 / `setvbuf` 行缓冲 / 异步日志后台线程持有文件。

4. **Q: 信号处理函数里能打日志吗？**
   A: 不能用 stdio/cout（非 async-signal-safe，可能持锁中被中断）；白名单里只有 `write` 等；更规范是只设 `volatile sig_atomic_t` 标志，由主循环打日志。

5. **Q: 什么时候需要异步日志？muduo AsyncLogging 怎么设计？**
   A: 磁盘 IO 阻塞业务线程时（刷盘、终端慢）。双缓冲：前端线程只往 currentBuffer `memcpy`（临界区极短），满了与 spareBuffer 交换，后台线程整块刷盘——避免逐条加锁，日志基本不丢。（RIS 在阶段 5 接入，到时候补实测数据。）

6. **Q: 你的日志为什么没有线程号？**
   A: 诚实答：当前还没到多线程业务场景，是已知演进点（`gettid()` 一行的事）；对比 muduo 自带 tid 在排障时定位并发线程的价值。

### 踩坑实录

- 【坑·乱序】PACS 日志里 `shutdown signal` 行出现在**最前面**：信号 handler 用无缓冲的 `write(2)`，INFO 行还在 stdio 缓冲区——**混用两条输出路径导致乱序**。已知技术债，阶段 1 重构状态机时改为"handler 只设标志位、主循环打日志"。
- 【坑·丢日志】RIS 日志文件一度全空（见追问卡第 3 条），临时用 `stdbuf -oL` 验证，正式解法是 AsyncLogging。

---

## 3. 配置模块

### 设计讲解

1. **三级合并：环境变量 > INI 文件 > 默认值。** 为什么是这个顺序：环境变量是容器/编排层注入配置和密钥的标准方式（12-Factor）；文件层适合团队共享的非敏感配置；默认值保证 clone 下来就能跑通本地开发。取值函数 `pick(env, ini, default)` 统一实现，新增配置项只加一行。
2. **凭据管理红线**：OSS 的 AK/SK **只**从环境变量注入，配置示例文件里刻意不写 OSS 节；日志输出一律打码（`mask()`，非空显示 `***`）。生产环境的正确姿势是 Vault / K8s Secret，而不是把密钥放进镜像或仓库。
3. **文件不存在 ≠ 错误，格式错误 = 启动失败。** 容忍缺文件（默认值兜底，本地开发友好），但对格式错误和校验失败 fail-fast——配置错误的最佳发现时机是启动那一刻，而不是运行到某条 SQL 才炸。报错带文件名+行号+原因。
4. **校验一次报全**：端口范围、必填项、条件必填（配了 `oss.endpoint` 则凭据必须齐全）。
5. **Config 是普通结构体、显式传引用，不用单例**：依赖注入，组件签名带 `const Config&`，可测试、无全局可变状态。单例的 Config 在单测里很难隔离。
6. **INI 而不是 YAML/JSON**：需求只是平面 KV，最小 INI 解析 ~60 行零依赖；YAML 解析复杂（缩进语义、类型推断）容易引入 bug，JSON 要引库（虽然 00_third_party 有 nlohmann 备用）。取舍讲清楚比无脑上 YAML 好。

### 追问卡

1. **Q: 为什么环境变量优先级最高？**
   A: 容器化部署里 env 是编排层（docker compose / K8s）注入配置和密钥的标准通道；临时改一个参数（如端口）用 env 不用改文件；12-Factor 的 config 原则。

2. **Q: 密码/密钥你怎么处理的？**
   A: 三层防线：不进代码库（示例文件不含真实凭据）、日志打码、生产走密钥管理系统。反例：把 AK 写死在代码里提交 GitHub 被扫出来盗刷——安全面试的高频开场。

3. **Q: 配置什么时候校验？为什么？**
   A: 启动时全量校验、一次报全、fail-fast。运行中发现配置错 = 半初始化状态 + 排障成本高。对比：懒校验（用到才查）会让错误暴露在流量高峰。

4. **Q: 配置热更新做过吗？**
   A: 诚实：当前需求不需要（启动期一次加载）。如果要做：配置双缓冲 + 原子指针切换 + 版本号通知（和 RIS 索引切换同一个思路），监听 SIGHUP 或配置中心 watch。能讲方案比硬吹做过加分。

5. **Q: 为什么 Config 不做成单例？**
   A: 显式依赖（构造函数传 `const Config&`）让组件可测试（传一个测试配置就能跑单测）、依赖可见（看签名就知道用了什么配置）；单例是隐藏的全局状态。

6. **Q: 你的 INI 解析器支持什么？不支持什么？**
   A: 支持：节、key=value、`;`/`#` 注释、首尾空白裁剪、Windows 换行 `\r`。不支持（有意）：多行值、转义、变量引用、include——需求里没有，YAGNI，等需要时再换 nlohmann/JSON。

### 踩坑实录

- 【坑·笔误被即时抓住】main.cpp 接入时误写了不存在的成员名，编译期报错立即修复——C++ 编译器是配置结构重构的第一道防线，成员名拼错逃不过链接期。
- 【设计取舍】横幅日志与 config_summary 一度重复输出连接信息——配置摘要统一为单条多字段日志（`mysql=root:***@host:port/db ...`），一处维护。

---

## 4. MySQL 连接池

### 设计讲解

1. **为什么需要池**：每次建连要走 TCP 三次握手 + 认证 + 会话初始化（毫秒级），高频建连既慢又消耗 MySQL 侧连接数（默认 `max_connections=151`）。池把建连成本摊销到进程生命周期。
2. **RAII 守卫 `MySQLGuard`**：构造获得、析构归还——异常路径（提前 return、抛异常）也不泄漏连接。**只可移动不可拷贝**：归还权唯一，防止同一连接被归还两次；`acquire()` 按值返回靠移动语义完成。这是 C++11 move 的实战用例。
3. **借出前 `mysql_ping` 校验 + 显式重连，刻意不开 `MYSQL_OPT_RECONNECT`**：自动重连会在连接断开时静默重建，可能丢事务上下文/会话变量、时机不可控（事务执行到一半换连接 = 数据错乱）。显式策略：ping 失败 → WARN 日志 → 同句柄重新 `mysql_real_connect` → 失败则销毁并补建新连接维持池大小。
4. **ping/重连在锁外执行**：数据库抖动时 `real_connect` 可能阻塞到 connect_timeout(5s)，若持锁执行会拖住所有等连接的线程——锁只保护 `idle_` 队列的出入队。
5. **acquire 带超时**：`condition_variable::wait_for` 3 秒拿不到就返回空 Guard + ERROR 日志，不无限阻塞业务线程（快速失败比挂死好排查）。
6. **utf8mb4**：MySQL 的 `utf8` 是残缺的 3 字节实现（emoji、部分生僻字存不下），患者姓名等中文场景必须 `mysql_set_character_set("utf8mb4")`。
7. **启动预热 + fail-fast**：`init()` 一次建满 pool_size 条，连不上直接退出进程——启动期发现问题的成本远低于运行期。
8. **为什么不用 workflow 自带的 WFMySQLTask 异步客户端**（必被问）：异步客户端彻底不阻塞 worker 线程，但结果集解析回调式、代码复杂度高；当前规模下同步池 + "IO 线程数略大于核数"够用；连接池是脱离 workflow 可移植的通用件。高并发演进路径：池换异步客户端、或 DB 前置队列削峰。能讲清代价交换比背答案重要。

### 追问卡

1. **Q: 连接池大小怎么定？**
   A: 不是越大越好：连接太多→MySQL 侧上下文切换、锁竞争反而劣化。经验起点：CPU 密集型 ≈ 核数，IO 密集型 = 核数 × (1 + 等待时间/计算时间)；同时受 MySQL `max_connections` 预算约束（多实例共享）。本项目默认 4，可配置。

2. **Q: MySQL 的 wait_timeout 断连怎么处理？**
   A: 服务端默认 8 小时空闲即断。方案对比：a) 借出前 ping（本项目，零后台线程）；b) 后台线程定期 ping 保活（池大时省 ping）；c) AUTO_RECONNECT（排除，见讲解 3）。

3. **Q: guard 为什么 delete 拷贝、只留移动？**
   A: 拷贝会让两个 guard 指向同一连接、析构时双重归还+并发使用；移动把"归还权"转移，旧对象置空。同时讲 move-and-swap 赋值实现。

4. **Q: 如果所有连接都失效且重建失败，池会怎样？**
   A: 已知限制（诚实讲）：acquire 返回空 Guard，且该连接从池中永久流失——极端情况池会耗干。演进点：后台保活线程 + 池低水位补建。当前在 acquire 超时日志里可见征兆。

5. **Q: 一次 HTTP 请求的连接生命周期？**
   A: handler 入口 acquire → 执行 SQL（可能多条）→ guard 析构归还 → cv.notify_one 唤醒等待者。连接绝不跨请求缓存——池化的是"连接"不是"会话"。

6. **Q: mysql_query 返回后为什么还要 mysql_store_result？**
   A: mysqlclient 默认不缓冲结果集；SELECT 必须取回（store/use_result）否则连接处于"未读净"状态，下一条查询报 Commands out of sync。/db/ping 里即使只要连通性也要把结果取空。

### 踩坑实录

- 【坑·真实演练】验证重连逻辑时直接 `docker restart` MySQL 容器：服务日志出现 `Lost connection to MySQL server during query` WARN → 自动重连 → 接口恢复 `mysql ok`，全程无人工干预。这是面试可复述的完整故事："我们怎么发现断连问题的（定时任务失败日志）→ 怎么定位（MySQL wait_timeout/容器重启）→ 怎么修（ping+显式重连）→ 为什么不用 AUTO_RECONNECT"。
- 【坑·笔误】LOG_ERROR 宏的字符串字面量没闭合，编译期即被抓——流式日志宏里字符串必须完整，多行拼接用 `<<` 续接而不是断引号。

---

## 5. 四层影像数据模型

> 完整建表 SQL：`sql/pacs_db_init.sql`（注释即讲稿）；重置：`bash scripts/init_db.sh`

### 设计讲解

1. **为什么四层而不是一张大表**：DICOM 标准本身就是 Patient→Study→Series→Instance 层级；查询模式不同（患者维度查历史检查 vs 实例维度定位文件）；范式化后患者信息只存一份。反例：一张大表里患者姓名在每个实例行重复，患者改名要更新几十万行，且历史行新旞性别不一致。
2. **代理键 + 业务唯一键的双键设计**：每层自增 `BIGINT id` 做主键（JOIN 用），DICOM UID 做唯一键（幂等用）。为什么不用 UID 直接当主键：UID 是 VARCHAR(64)，InnoDB 二级索引叶子存主键值，长主键让所有二级索引膨胀；自增主键还保证聚簇索引顺序插入，避免页分裂。
3. **患者身份 = (PatientID, Issuer) 复合唯一**，且 issuer `NOT NULL DEFAULT ''`——MySQL 唯一索引对 NULL 不去重（已实证：两条 NULL 都能插入），空串才参与唯一约束。同一 PatientID 不同发行方 = 不同患者。
4. **uk_sop_uid 是幂等的地基**：并发重复导入时第二个 INSERT 撞 1062 → 转为"重查后按哈希决策"。对比"先查后插"：两个线程都查不到 → 都插入 → 脏数据；唯一索引把竞态收窄到数据库层原子裁决。
5. **物理外键（本项目的取舍）**：写入方单一、量级可控、医疗数据完整性优先 → 加 FOREIGN KEY；互联网高并发/分库分表场景常用应用层维护外键（性能与拆库灵活性）。能对比着讲比站队重要。
6. **状态机持久化在 status 字段**：RECEIVED→PARSED→ARCHIVED + 终态 DUPLICATE/CONFLICT/FAILED。放数据库而非内存是"中断续传"的前提——进程崩溃重启后靠 status 判断哪些对象未完成。
7. **CHECK 约束防脏状态**（MySQL 8.0.16+ 才真正生效，之前解析但忽略——版本坑）。
8. **细节弹药**：sha256 用 CHAR(64)（定长十六进制）；DATETIME(3) 毫秒精度排障；storage_path 存相对路径（换存储介质不刷库）；模态冗余在 series 层（DICOM 中 (0008,0060) 是序列级属性，study 级"有哪些模态"靠聚合，不冗余避免不一致）；医疗数据不做物理删除（法规要求可追溯）。
9. **本地消息表 backup_event 与归档同库**：这是"与归档状态同事务提交"的前提（跨库就没有本地事务可用）——为什么消息表跟业务表放同一个库，面试讲事务边界时是关键一句。

### 追问卡

1. **Q: 表结构怎么设计的？为什么这样拆？**
   A: 按 DICOM 四层层级 + 各查询模式拆表；讲双键设计（代理键 JOIN、UID 唯一幂等）；举一张大表的反例（改名更新风暴）。

2. **Q: 为什么不直接用 StudyInstanceUID 当主键？**
   A: 长字符串主键 → 二级索引膨胀 + 随机插入页分裂；自增主键 + UID 唯一键兼得两者好处。

3. **Q: 幂等是怎么保证的？并发重复导入会怎样？**
   A: uk_sop_uid 唯一索引原子裁决，1062 后走"重查 + 哈希对比"分支；对比先查后插的竞态窗口。（完整决策树在阶段 1 实现。）

4. **Q: 外键你们加不加？**
   A: 加（本项目：完整性优先、写入方单一）；同时能讲互联网场景为何不加（高并发锁竞争、分库分表跨库 FK 失效）。

5. **Q: CHECK 约束有什么坑？**
   A: 8.0.16 前被解析但静默忽略——升级库版本后"突然生效"可能把老 ETL 打挂；应用层校验不能省。

6. **Q: 为什么要本地消息表？直接发 MQ 不行吗？**
   A: 直接发存在"DB 提交成功但消息没发出"的丢失窗口（或反之的幽灵消息）；本地消息表把"记录待发事件"和业务状态变更放进同一本地事务，配合补偿扫描保证最终必发。（细节在阶段 2 展开。）

### 踩坑实录

- 【实证】NULL 陷阱：建临时表验证 `UNIQUE(pid, issuer)` 下两条 `issuer=NULL` 均插入成功（COUNT=2）——所以生产表用 `NOT NULL DEFAULT ''`。面试时这个坑能现场画 SQL 演示。
- 【坑·shell】zsh 不对未加引号的变量做分词，`$M -e "..."` 报 command not found——改函数封装。跨 shell 脚本要写 `#!/bin/bash` 显式声明（scripts/ 下已是这么做的）。

---

## 6. 手写 DICOM 解析器

> 代码：`03_/src/dicom/DicomReader.cpp`；测试数据：`scripts/gen_dicom.py` → `testdata/dicom/`
> 自测：`./build/bin/pacs_archive_service --parse testdata/dicom/01_ok_explicit.dcm`

### 设计讲解

1. **DICOM Part-10 文件结构**（能画出这个图就有分）：
   `[128B preamble(任意内容)] ["DICM"魔数] [文件元组 group 0002] [数据集]`
   文件元组**恒为显式 VR 小端**（标准规定，与传输语法无关）；数据集编码由元组里的 TransferSyntaxUID 决定。
2. **传输语法分派**：`1.2.840.10008.1.2`=隐式 VR 小端；`.1.2.1`=显式 VR 小端；`.1.2.4.x`(JPEG 系)/`.1.2.5.x`=压缩语法但**数据集仍是显式小端**（压缩只影响像素内部）；`.1.2.2`=Big Endian（2008 起废弃）→ 明确拒绝。
3. **像素零扫描策略（性能核心）**：Part-10 要求数据集内 tag 升序，我们关心的元数据 tag 数值全部 < (7FE0,0010)，所以遇到像素数据立即停止——解析耗时与文件大小解耦，500MB 的 CT 只扫前几 KB。
4. **显式 VR 长短形式**：普通 VR 是 `VR(2B)+u16长度`；OB/OW/OF/OD/OL/SQ/UC/UR/UT/UN 等 12 个长形式 VR 是 `VR(2B)+2B保留+u32长度`。隐式 VR 则恒为 `tag+u32长度`（跳过时与 VR 无关，反而更简单）。
5. **未定长 SQ**：长度 0xFFFFFFFF 的序列用"深度计数 + 递归"跳到 (FFFE,E0DD)；项和结束符（group FFFE）是**没有 VR 字段**的特殊编码，需单独分支。
6. **安全底线**：所有读取走带边界检查的游标（`rd_u16/rd_u32/skip_bytes` 全部检查余量），截断/巨长长度统一报错，绝不越界读——解析的是不可信输入（上传文件），一次越界读就是崩溃或漏洞。
7. **小端假设**：数值按 memcpy 直读，依赖宿主小端（x86_64/aarch64 容器均满足）；跨大端平台需改造读取函数——字节序意识。
8. **为什么不用 DCMTK**：依赖重、API 面广；本项目只需提取四层元数据，~300 行自写可控可讲可调试；如果需求扩展到像素解码/DCM 转 JPEG，再引 DCMTK 也不迟。

### 追问卡

1. **Q: 讲一下 DICOM 文件结构。**
   A: preamble + DICM + 元组(恒显式小端,含传输语法) + 数据集(编码由 TS 决定)；顺带讲四层层级与 UID。

2. **Q: 显式 VR 和隐式 VR 的区别？**
   A: 显式带 2 字节 VR 类型 + 长短两种长度形式；隐式无 VR、恒 u32 长度，VR 要查字典才能知道语义——跳过不需要字典，取值才需要。

3. **Q: 500MB 的 CT 文件解析要多长时间？**
   A: 近乎常数时间——tag 升序 + 元数据 < 像素 tag → 遇 (7FE0,0010) 即停，像素区零扫描。这个策略是自写解析器最漂亮的取舍。

4. **Q: 上传一个恶意构造的 DICOM（长度字段写 4GB）会怎样？**
   A: 游标边界检查在读取长度后立即比对剩余字节数，越界直接报 TRUNCATED 拒绝，不会分配/拷贝。

5. **Q: 序列（SQ）里嵌了同名的患者姓名 tag，你会读错吗？**
   A: 不会——SQ 整体跳过（已用嵌套+假 tag 的测试文件实证：内部 WRONG-NAME 未泄漏，顶层读到张三）。

6. **Q: 中文患者名怎么通的？**
   A: PN 的值就是字节串，utf-8 直通；入库侧连接池设 utf8mb4、表也是 utf8mb4，全链路不转码。（MySQL utf8 是 3 字节残缺实现的老坑。）

### 踩坑实录

- 【坑·tag 写错】建表 SQL 注释里把 PatientSex 写成 (0080,0100)，写解析器核对标准时发现应为 (0010,0040) 已修正——**讲 tag 必须核对原始标准，背错比不背更减分**。
- 【坑·Python】生成器里 `None.encode()` 崩溃——编码职责应收敛到辅助函数（`enc()` 先判 bytes/str），让 None 过滤先生效；顺手删掉两处死代码（`if False` 分支、占位函数）。

---

## 待补清单（随开发更新）

- [ ] 阶段 1：状态机 + 信号 handler 重构（修乱序坑）后更新本节
- [ ] 阶段 5：AsyncLogging 接入后补实测与双缓冲细节
- [ ] 阶段 7：把本文件与追问树合并成完整面试包（2–3 分钟项目叙事 + 三档回答）

---

## 7. 导入主链路：事务与幂等决策树（阶段 1）

### 设计讲解

1. **两事务设计**：事务一（决策+层级 upsert+instance PARSED）与事务二（ARCHIVED+本地消息表同事务）分离，中间是 rename 文件的操作——**不能把文件操作放进数据库事务**（事务里做 IO 会拉长锁持有时间）。崩溃窗口由 PARSED 状态 + staging 文件兜底。
2. **层级 upsert 一条 SQL**：`INSERT ... ON DUPLICATE KEY UPDATE id=LAST_INSERT_ID(id), name=VALUES(name)`——`mysql_insert_id` 在新插入和撞唯一键两种情况都返回行 id，无需先查后插（消竞态）。这是 MySQL 的惯用法。
3. **决策树在 SELECT ... FOR UPDATE 之后**：锁住该 UID 行（唯一索引），并发同 UID 导入在此排队，两个事务不可能同时判"不存在"。DUPLICATE/CONFLICT 分支不改既有数据（不因错误上传污染已归档数据）。
4. **staging 与最终对象同目录**：`rename(2)` 原子且不跨文件系统（跨了会 EXDEV 失败）——staging 放 /tmp 是常见错误。
5. **同 UID 异哈希 = 冲突拒绝**（409），不静默覆盖——医疗数据的不可变性要求。

### 追问卡

1. **Q: 并发导入同一文件会怎样？** → FOR UPDATE 串行化决策；后到者撞 DUPLICATE 分支幂等返回。
2. **Q: 为什么不一个大事务包揽？** → 文件 rename 是 IO，事务里做 IO = 长事务 + 长锁；拆开后崩溃窗口有状态机兜底。
3. **Q: 崩溃在哪些点会发生什么？** → 三个窗口逐个讲（写 staging 中/事务一后 rename 前/rename 后事务二前），各自恢复路径。
4. **Q: ON DUPLICATE KEY UPDATE 的坑？** → 影响的行数语义（1=插入 2=更新 0=无变化）；LAST_INSERT_ID(id) 技巧的前提是表有自增 id。

### 踩坑实录

- 【坑·真 bug】`mysql_insert_id()` 在 `COMMIT` 之后调用返回 0（"最后一条语句"变成了 COMMIT）→ 事务二拿 instance_id=0 插消息表被外键拒绝 → 实例卡 PARSED，第二次导入靠续传分支"自愈"。修复：INSERT 后立即取 id 再 COMMIT。**这个 bug 的发现-定位-修复全程是现成面试故事**。
- 【坑】`std::endl` 隐式 flush 让旧代码"看起来没问题"，换掉后缓冲问题才暴露。

---

## 8. 异步备份链：本地消息表 + RabbitMQ 可靠投递（阶段 2）

### 设计讲解

1. **为什么本地消息表**：直接发 MQ 有"DB 提交成功但消息没发出"（丢）或反过来的窗口。把"记录待发事件"和业务状态变更放进**同一本地事务**，配合补偿扫描，保证**最终必发**。这是分布式事务的"事务消息"方案（对比：RocketMQ 事务消息、Saga、outbox 模式同族）。
2. **publisher confirm 用裸 rabbitmq-c 实现**：SimpleAmqpClient 2.5.1 没暴露 confirm API——发布端 confirm 是"broker 确认落盘"的关键，值得手写（confirm.select → 等 basic.ack，3s 超时）；消费端用 SimpleAmqpClient（重连友好）。
3. **消费端幂等收敛**：at-least-once 投递 + 消费侧三道闸：①task_id 已 CONFIRMED 直接 ack 跳过 ②OSS PutObject 同 key 同内容覆盖天然幂等 ③更新两个状态同一事务。净效果 exactly-once。
4. **有界重试 + 指数退避**：失败后 retry_count+1、next_retry_at=NOW()+2^n 秒，由主服务扫描线程补发；5 次超限置 DEAD+FAILED 终态。
5. **死信队列的位置**：毒消息（JSON 解析失败）走 basic.reject(requeue=false) → 队列的 x-dead-letter-exchange 路由到 DLQ 人工排查；业务失败走重试不走 DLQ——两条失败路径刻意分开。
6. **消费者独立进程**：可单独重启演示"杀掉→消息堆积→重启不丢不重"；生产上按积压独立扩容。

### 追问卡

1. **Q: 消息会不会丢？列出所有窗口。** → ①事务二前崩溃：事件根本不存在，无消息可丢 ②发布/confirm 前崩溃：PENDING 超 5s 被扫描补发 ③broker 落盘前宕机：消息+队列都持久化(delivery_mode=2+durable) ④消费处理中崩溃：手动 ack 未发，redelivery。
2. **Q: 消息会不会重复消费？** → 会（at-least-once），靠 task_id 查状态跳过 + OSS 覆盖幂等收敛。
3. **Q: 为什么不用 RocketMQ/Kafka？** → RabbitMQ 的 confirm/DLQ/路由模型完整支撑需求且团队熟悉；Kafka 吞吐优势在日志流场景，此处消息量小但可靠性要求高。
4. **Q: 扫描线程会不会和快路径重复发？** → 会短暂可能（confirm 成功但标记 PUBLISHED 前崩溃），重复消息由消费端幂等吸收——**允许重复，保证不丢**。
5. **Q: DLQ 里的消息怎么处理？** → 人工排查（毒消息意味着上游 bug），修复后可重新投递。

### 真实云端全链路验证（2026-08-16，cn-wuhan-lr）

四幕全部实测通过：①导入→confirm→消费→PutObject→BACKED_UP+CONFIRMED
②kill -9 消费者→消息堆积→重启后积压消化不丢不重 ③毒消息 rabbitmqadmin 注入→
BasicReject→DLQ 计数=1 ④签名 ListObjects 确认云端对象字节数与源文件一致。

### 踩坑实录

- 【坑·路径双拼】storage_path 入库带根目录前缀 + 消费端再拼一次 → `data/storage/data/storage/...`。修复：库里只存相对 storage 根的路径（studyUID/seriesUID/sopUID.dcm），读写两端各自拼根——"换存储介质不刷库"的设计本来就该这样。
- 【坑·排他消费僵尸】SimpleAmqpClient 2.5.1 的 BasicConsumeMessage 无超时无限阻塞，SIGTERM 软退出标志永远等不到检查 → 僵尸进程占住 exclusive consumer，后续实例全部 403 ACCESS_REFUSED。修复：信号处理器直接 _exit——未 ACK 消息会被 broker 重投，幂等消费吸收重复，**幂等设计反过来简化了关闭语义**（面试好句）。
- 【坑】SimpleAmqpClient 2.5.1 没有 BasicNack 也没有超时版 BasicConsumeMessage——用 BasicReject(requeue=false) 等效走路由到 DLX。
- 【坑·OSS 签名】手写签名 ListObjects 三次 403：canonical resource 必须含 bucket 名（/{bucket}/），list-type/prefix 等非子资源不参与签名——SDK 屏蔽的细节手写时全是坑。
- 【坑】命名空间是 `AmqpClient` 不是 `SimpleAmqpClient`（库名与命名空间不一致，头文件路径 SimpleAmqpClient/...）。

---

## 9. 认证微服务：srpc + Consul + JWT + RBAC（阶段 3）

### 设计讲解

1. **为什么拆独立服务**：认证是横切能力，独立部署可被多个网关复用；技术栈上 srpc(Protobuf 二进制) 比 HTTP-JSON 更省带宽且代码生成强类型。
2. **PBKDF2 而非明文/单层 SHA**：10 万次迭代 + 16 字节随机盐，抵抗彩虹表与 GPU 暴力枚举；每个用户独立盐。
3. **JWT 手写 HS256**：三段 `b64url(header).b64url(payload).HMAC-SHA256`；验签用 `CRYPTO_memcmp`（常数时间比较，防时序侧信道逐字节猜签名）；exp 校验。为什么不存 session：无状态、网关本地即可验（演进方向：非对称 RS256 支持多服务验签）。
4. **RBAC 矩阵在认证服务端**：role→action 集中一处（radiologist: images:import+studies:read; admin: 全部），网关只传 action 不做判断——权限变更不动网关代码。
5. **服务发现三级回落**：env 显式指定 > Consul health 查询（剔除不健康实例）> 默认地址；调用失败置 resolved_=false 下次重查——服务漂移自愈。
6. **登录错误文案统一**"用户名或密码错误"：不暴露用户是否存在（用户枚举防护）。

### 追问卡

1. **Q: JWT 怎么注销/续期？** → 诚实答：当前短 TTL(2h) 自然过期；注销需黑名单（Redis 记 jti）或 refresh token 双令牌——讲清演进路径。
2. **Q: JWT secret 泄露怎么办？** → 轮换 secret（旧 token 全失效）；非对称签名把验签公钥公开、签发私钥隔离。
3. **Q: 为什么 TCP 健康检查不是 HTTP？** → srpc 是二进制协议没有 HTTP 端点；TCP 探活足够判断进程+端口存活（应用层健康需业务心跳，演进点）。
4. **Q: 网关每次请求都调认证服务，性能？** → 同步 RPC ~1ms 局域网；演进：网关本地验签（JWT 本来就支持）+ 只在权限变更时调 Verify；或加本地权限缓存 TTL。
5. **Q: PBKDF2 为什么不用 bcrypt？** → OpenSSL 原生提供 PBKDF2 无需引依赖；两者安全性同档，argon2 更现代但引入新库。

### 踩坑实录

- 【坑】`--login` 工具 stdout 混入日志 → token 变量带换行 → 请求头非法 400。修复：Logger 支持 set_log_output(stderr)，CLI 工具 stdout 只有机读结果。**"工具的 stdout 是 API"** 是工程习惯。
- 【坑】ppconsul 0.2.3 的 Health/Agent 都是独立构造式（`Health h(consul)`），不是 consul.health() 访问器；Tags 是 std::set 不是 vector。

---

## 10. RIS 离线：解析、分词、TF-IDF 倒排索引（阶段 4）

### 设计讲解

1. **坏文件不致命**：解析失败的报告进 bad 列表统计（构建日志输出"坏文件 N"），不阻断整批构建——数据质量报告是离线管道的基本要求。
2. **分词必须索引/查询同源**：Tokenizer 类唯一封装 cppjieba(Mix) + 停用词过滤——两边分词不一致是检索召回劣化的经典根因。
3. **TF-IDF + L2 归一化在构建期完成**：文档向量归一化后，余弦相似度退化为点积；conclusion 权重 ×2、description ×1（主字段更重要，简化版 field weighting）。
4. **不可变快照 + atomic_store(shared_ptr)**：构建期新版本、检索期旧版本互不干扰；旧版本引用计数归零自动析构——"索引切换"零锁。
5. **版本目录落盘**：v1/v2/... 共存，加载取最大版本——回滚 = 指回旧目录；重建不锁服务。

### 追问卡

1. **Q: 为什么 TF-IDF 不是 BM25？** → TF-IDF 是教学最短路径且效果可接受；BM25 的饱和函数/文档长度归一是明确演进点（能讲出 BM25 两处改进就有分）。
2. **Q: 索引多大？内存多少？** → 50 文档 182 词项（演示级）；讲扩展：倒排可 mmap/分片，词典可 FST。
3. **Q: 增量更新怎么做？** → 当前全量重建到新版本目录（分钟级，可接受）；真增量要文档级 tombstone + 定期 merge——和 Lucene 段合并同思路。
4. **Q: 停用词怎么定的？** → cppjieba 自带中文停用词表 + 过滤单字节 ASCII 噪声；领域停用词（"患者""检查"）可从高 df 低区分度词统计中挖掘。

### 踩坑实录

- 【坑】TSV 序列化时结论含换行/制表符 → 转义函数必须先行（\n \t \\ 三种），读取端对称反转义。
- 【坑】`system("mkdir -p")` 的注入面：目录名来自内部版本号拼接才安全；外部输入绝不能拼 shell 命令。

---

## 11. RIS 在线：TLV 协议、muduo 集成、检索执行（阶段 5）

### 设计讲解

1. **TLV 帧格式设计**：`[type:1B][flags:1B][len:4B 网络字节序][payload]`——4 字节长度字段上限 4GB，主动限制 1MB 防内存攻击。**Length 按网络字节序**：小端宿主必须手动拼字节，memcpy 直读会错（字节序考点的实物）。
2. **增量解码器**：内部缓冲 + 状态机（头部不足→PARTIAL 等数据；体不足→PARTIAL；非法 type→BAD_TYPE 断连；超长→TOO_LONG 断连）。半帧测试：三段 send 拼一帧。
3. **为什么自定义协议不是 HTTP**：内网常驻服务、二进制头 6 字节 vs HTTP 头几百字节；检索是高频短请求，协议开销占比高。
4. **每连接一个解码器实例**（按连接名挂 map）：连接断开时清理——连接级状态的生命周期跟着连接走。
5. **muduo 的 send 线程安全**：跨线程调用会转交 IO 线程（讲 muduo 内部实现：runInLoop）。
6. **摘要的 UTF-8 边界对齐**：字节截断会切坏多字节字符 → JSON 非法——向前/向后跳过 10xxxxxx 续字节。

### 追问卡

1. **Q: 粘包/半包怎么处理？** → TLV 有长度字段：解码器循环取完整帧，不完整留缓冲。这是"为什么 TCP 需要 应用层协议"的标准答案。
2. **Q: 客户端发 2GB 长度字段？** → MAX_FRAME_PAYLOAD=1MB 检查 + 断连；否则缓冲区爆炸。
3. **Q: 为什么断连而不是跳过坏帧？** → 非法 type 时长度字段不可信，无法定位下一帧起点——同步已不可能。
4. **Q: TopK 为什么用小顶堆？** → 维护 size=k 的堆，O(n log k)；全排序 O(n log n) 浪费。

### 踩坑实录

- 【坑·两连击】摘要按字节截断产生非法 UTF-8：①term 命中路径 ②term 未命中路径的 substr(0,48)——两处都要对齐码点边界；修完后**Redis 里还缓存着坏结果**（TTL 300s），FLUSHALL 才见到修复效果。"改了代码没生效？先想缓存"。
- 【坑】中文编辑距离按字节算：一字 3 字节，"肺节结"→"肺结节"距离 3 超过 max_dist=2 → 纠错空结果。改用 utfcpp 切码点后距离 2 正常召回——多字节文本的字节操作都要过一遍这个检查。

---

## 12. RIS 缓存：LRU + Redis 版本化键（阶段 6）

### 设计讲解

1. **两级缓存分工**：L1 LRU（进程内，~100ns）挡高频热词；L2 Redis（跨进程共享，~1ms）挡多实例重复计算；miss 才走索引（分词+检索 ~10ms 级）。
2. **LRU 实现**：mutex + list（访问序）+ unordered_map（key→节点）；get 命中 splice 到队头 O(1)，put 满淘汰队尾 O(1)——面试手写题本体。
3. **版本化缓存键**：`ris:v{版本}:{FNV-1a(查询)}`——索引切换后新查询落新键，旧键靠 TTL(300s) 自然过期，**不需要主动清理**；已实证 Redis 中 v1/v2 键共存。
4. **FNV-1a 而非 std::hash**：跨进程/跨编译器稳定的 64 位哈希才能做共享缓存键（std::hash 实现相关）。
5. **降级策略**：Redis 不可用时静默失败（告警一次），检索直查索引——缓存是加速器不是依赖。

### 追问卡

1. **Q: 缓存一致性？** → 结果缓存的失效源是索引重建——版本号进键从根上解决；报告数据变更走新版本目录。
2. **Q: 缓存穿透/雪崩？** → 穿透：不存在的词每次都查索引（可加空结果短 TTL 缓存）；雪崩：TTL 加随机抖动；热点：L1 挡住。
3. **Q: 为什么 LRU 不是 LFU？** → 查询热点随病种/时间漂移，LRU 贴合近期性；LFU 有老化问题。
4. **Q: 布隆过滤器用得上吗？** → 词表 182 个不需要；词典 10 万+ 时可在索引前挡"词不存在"的查询。

### 实证记录

- L2 命中日志：`L2 命中(Redis): 骨折`；L1 命中：`L1 命中: 肺结节`；版本切换后 Redis 键 `ris:v1:2cb1ef0a...` 与 `ris:v2:2cb1ef0a...` 共存——简历句子逐字对应。

---

## 13. Web 控制台 + RIS TLV 桥接：workflow 自定义协议客户端（阶段 7，2026-08-16）

### 设计讲解

1. **动机**：浏览器无法直连 RIS 的 TCP 9090（TLV 二进制协议）。选择在 PACS 服务内写 C++ 桥接（`src/risproxy/`），把 `GET /api/v1/reports/search|suggest` 翻译成 TLV 帧打到 RIS——两个后端首次真正打通，"归档服务作为客户端按 TLV 协议调用检索服务"从面试话术变成真实代码。
2. **workflow 自定义协议机制**：`RisTlvMessage : public protocol::ProtocolMessage`，实现 `encode()`（发送方向：6 字节头 + payload 装进两个 iovec 零拷贝）和 `append()`（接收方向：增量半包状态机，`size_t*` 出参告知框架消费字节数）——官方 tutorial-10 同款模式。客户端任务用 `WFNetworkTaskFactory<RisTlvMessage, RisTlvMessage>::create_client_task(TT_TCP, ...)` 创建，`set_keep_alive(60s)` 让 workflow 连接池复用长连接（对比 test_ris_client.py 每查询新建连接）。
3. **全异步桥接（核心卖点）**：wfrest handler 支持第三参 `SeriesWork* series`——handler 里把 TLV 客户端任务 `series->push_back(task)`，HTTP 响应推迟到 TLV 回调里写。整条链路（收 HTTP → 发 TLV → 收 TLV → 回 HTTP）不阻塞任何 handler 线程，这是 workflow Task/Series 异步编排的标准用法，也是"为什么不用 muduo 写 HTTP"追问的现成论据。
4. **同源静态托管免 CORS**：`svr.Static("/ui", "./06_web")` 把前端挂在同一 8080 端口，浏览器请求无跨域，后端零 CORS 头代码。wfrest 的 Static 只映射 `GET /ui/*` 不出目录首页，补了 `/ui` 和 `/ui/` 两个 302 跳转。
5. **登录链路补全**：原来拿 JWT 只能 CLI `--login`。新增 `POST /api/v1/auth/login`（srpc 同步调认证服务，与 AuthGate::authorize 现状一致）；响应不含 role（LoginResponse 协议没这字段），前端从 JWT payload 段 base64url 解码取 `sub/role/exp`——顺便演示了 JWT 三段结构。
6. **RBAC 零侵入**：RIS 桥接接口复用现有 action `studies:read` 鉴权，认证服务的 RBAC 矩阵一行不改；admin/radiologist 都能检索，备份页仍仅 admin。
7. **后端纯增量**：既有路由/归档流水线/DAO/Config 零改动，main.cpp 只追加代码块——`git diff` 可逐行验证（这是和"改后端逻辑"权衡后的明确决策）。
8. **前端零依赖**：原生 HTML/CSS/JS + hash 路由单页（06_web/ 三件套），不引入 Node 构建链——C++ 面试项目的前端定位是"演示后端能力的壳"，离线可开、刷新即生效。

### 追问卡

1. **Q: 浏览器怎么连你的 TCP 服务？** → 连不了也不该连：在 HTTP 网关层做协议桥接，TLV 编解码在服务端完成，前端只消费 JSON。备选方案是 WebSocket 网关或 RIS 加 HTTP——前者多一条长连接链路，后者要动 muduo 服务。
2. **Q: 桥接为什么是异步的？同步调行不行？** → handler 里同步等 RIS 会占住 wfrest 的 handler 线程，RIS 慢查询时直接拖垮 HTTP 并发；SeriesWork 串联让等待发生在 workflow 调度器里。（对比：登录接口是同步 srpc——低频且 localhost，属有意识的取舍。）
3. **Q: TLV 客户端怎么处理粘包/半包？** → 和服务端同款思路：`append()` 增量状态机——头部 6 字节不齐先攒，齐了按大端读长度，体不齐继续攒；帧类型白名单（0x11/0x12/0x7F）+ `set_size_limit(1MB)` 防内存攻击。
4. **Q: RIS 挂了会怎样？** → 连接失败 → 任务 state != SUCCESS → 映射 HTTP 502 + `WFGlobal::get_error_string` 错误详情；查询幂等设了 retry=1；workflow 连接池在 RIS 恢复后自动重连（实测：杀掉 RIS 再拉起，无需重启 PACS）。
5. **Q: 前端会话怎么管？** → JWT 存 localStorage，每次请求带 `Authorization: Bearer`；401 全局拦截清会话弹登录；过期靠 JWT exp 前端预检。追问 XSS 偷 token：localStorage 确实比 httpOnly cookie 风险高，演示项目接受，生产可换 cookie+CSRF token。
6. **Q: 为什么静态资源不放 CDN/nginx？** → 单二进制自包含部署更贴合项目演示定位；wfrest Static 自带 MIME 与文件服务，生产再拆。

### 踩坑实录（5 个，全是当天实测）

1. **wfrest `query()` 不做百分号解码**：前端 `encodeURIComponent` 的中文查询串原样透传，RIS 收到 `%E7%A3%A8...` 字面量，命中 0 条——症状极其隐蔽（HTTP 200 正常返回）。修法：桥接 handler 里手写 `url_decode`（含 `+`→空格）。
2. **`resp->Json("字面量")` 编译歧义**：wfrest::Json 存在 `const char*` 隐式构造，同时匹配 `Json(const Json&)` 和 `Json(const std::string&)` 两个重载——字符串字面量必须显式 `std::string(...)`。
3. **wfrest::Json::parse 非法输入是空指针陷阱**：parse 坏 JSON 得到 `node_=null` 的对象，`has()/get()` 会走到 `json_value_type(nullptr)` 直接解引用崩溃。登录接口三重校验 `is_valid() → has() → is_string()` 才取值。
4. **wfrest Static 不出目录首页**：`/ui/` 映射到目录而非 index.html；补 `/ui`、`/ui/` 两个 302 到 `/ui/index.html`。
5. **pkill 自杀坑再现**：容器内脚本 `pkill -f "bin/xxx"` 会匹配到自己所在的 `bash -c` 命令行（内含模式串）导致 SIGTERM 自杀——必须 `$` 锚定（usage_guide 第七节早有记载，这次又被咬了一口）。

### 已知限制（面试坦诚项）

- 静态根目录相对 CWD，须从仓库根目录启动（启动日志有提示与 WARNING）。
- `/api/v1/studies` 无分页（LIMIT 200 硬编码），前端全量渲染。
- RIS 搜索结果不含 study_instance_uid，`has_image` 只能出徽章、无法深链到 PACS 检查详情（可列为增量点：RIS 响应 JSON 加字段）。
- 登录接口同步 srpc 占用一个 handler 线程（与 AuthGate.authorize 现状一致）。

### 实证记录

- curl 全套通过：登录 200 拿 token / 无 token 401 / 错密码 401 / doctor 检索 200 / doctor 备份 403 / 缺 query 400 / 杀 RIS → 502 "Connection refused" → 拉起后自动恢复 200。
- GUI（浏览器自动化）走查通过：admin 全 7 页（含"磨玻璃"检索 5 条、高亮、has_image 徽章、BK-tree 纠错建议条、备份流水 CONFIRMED）、doctor 菜单裁剪 + 直访备份页权限卡、`/ui` 302、刷新后 JWT 会话保持。

---

## 待补清单（随开发更新）

- [x] OSS 消费端实测（真实阿里云 cn-wuhan-lr：四幕全过，云端对象已确认，见第 8 节）
- [ ] 压测（wrk2 导入接口 / RIS 检索 QPS）与基准数据
- [x] 2026-08-16：全部核心模块完成（第 7-12 节弹药补齐）
- [x] 索引版本切换与缓存版本化实证（第 12 节）
- [x] 2026-08-16：Web 控制台 + RIS TLV 桥接完成（第 13 节，curl + GUI 双实证）
- [ ] RIS 搜索响应补 study_instance_uid → 前端 has_image 徽章可深链到 PACS 检查详情（跨系统联动增强）
