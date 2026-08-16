# 一档模块闭卷重写需求文档（tier1_rewrite_specs）

> 用法：按三档学习策略，一档模块**先不看参考实现**，只读本文件的需求与验收标准，
> 独立写完后与 `03_/`、`04_/` 对应文件 diff，差异点回到 `interview_ammo.md` 对应章节。
> 每个模块标注了参考实现文件与建议时长。

---

## 1. MySQL 连接池（建议 90 分钟）

**参考**：`03_/include/db/MySQLPool.h` + `src/db/MySQLPool.cpp`

### 需求
1. `MySQLPool(MysqlConfig)` 固定大小池，`init()` 启动预热 N 条连接（fail-fast）
2. `acquire(timeout_ms)` 阻塞获取，超时返回空 Guard；内部 mutex + condition_variable
3. `MySQLGuard` RAII 守卫：析构自动归还；**禁拷贝、支持移动**（防双重归还）
4. 借出前 `mysql_ping` 校验；失效则同句柄重连；再失败销毁并补建（池大小不变）
5. 不使用 `MYSQL_OPT_RECONNECT`；utf8mb4；connect timeout 5s
6. ping/重连必须在锁外执行（想清楚为什么）

### 验收
- [ ] kill -INT 退出时连接全部关闭（日志可见）
- [ ] docker restart mysql 后 acquire 自动恢复（WARN 日志 + 业务无感）
- [ ] Guard 移动语义测试：vector<Guard> push_back 不编译报错、不双重归还

---

## 2. TLV 增量解码器（建议 60 分钟）

**参考**：`04_/include/protocol/TlvCodec.h` + `src/protocol/TlvCodec.cpp`

### 需求
1. 帧格式 `[type:1B][flags:1B][len:4B 网络字节序][payload]`
2. `feed()` 追加字节流；`decode()` 循环出帧，返回 OK/PARTIAL/BAD_TYPE/TOO_LONG
3. 半帧留在内部缓冲；连续多帧一次解出；len > 1MB 判 TOO_LONG
4. 长度字段手动拼大端（禁止 memcpy 直读）
5. 编码函数 `encode_frame(type, payload)`

### 验收
- [ ] 用 `scripts/test_ris_client.py` 的场景自测：三段 feed 拼一帧、一次 feed 两帧、非法 type、0xFFFFFFFF 长度
- [ ] 说得出：为什么 BAD_TYPE 要断连而不是跳帧（长度字段不可信）

---

## 3. 本地消息表链路（建议 2 小时，最重的一档）

**参考**：`03_/src/archive/ArchiveDao.cpp`（两事务）、`src/mq/BackupPublisher.cpp`（confirm）、
`src/mq/BackupDispatcher.cpp`（补偿扫描）、`consumer/main.cpp`（消费幂等）

### 需求
1. 事务二：UPDATE instance→ARCHIVED 与 INSERT backup_event(PENDING) **同一事务**
2. 发布：confirm.select → publish(delivery_mode=2) → 等 basic.ack（3s 超时）；成功后置 PUBLISHED
3. 扫描线程（3s 周期）：PENDING 超 5s 或 PUBLISHED 且 next_retry_at 到期 → 补发
4. 消费端：毒消息 reject(requeue=false)→DLQ；task_id 已 CONFIRMED→跳过；OSS 成功→同事务双状态更新+ack；失败→retry_count+1 指数退避，5 次置 DEAD
5. 拓扑：direct exchange + durable queue + DLX/DLQ

### 验收（能白板画图+讲清每个窗口）
- [ ] 列出消息丢失的 4 个窗口及各自防线
- [ ] 解释"允许重复、保证不丢"如何收敛到 exactly-once 效果
- [ ] 为什么发布端裸 rabbitmq-c 而消费端 SimpleAmqpClient

---

## 4. DicomReader + 实例状态机（建议 2 小时）

**参考**：`03_/src/dicom/DicomReader.cpp`、`src/archive/ArchiveService.cpp`

### 需求（解析器）
1. 128B preamble + "DICM" 校验；文件元组（恒显式小端）读 TransferSyntaxUID
2. TS 分派：implicit/explicit LE 解析数据集；BE 拒绝
3. 显式 VR 长短形式（12 个长 VR）；未定长 SQ 深度计数跳过；(FFFE,xxxx) 无 VR
4. 遇 (7FE0,0010) 立即停止（tag 升序前提）；全程边界检查
5. 提取 16 个字段（四层 UID/PatientID/姓名/日期/模态/序号）

### 需求（状态机）
6. RECEIVED→PARSED→ARCHIVED + 终态 DUPLICATE/CONFLICT/FAILED
7. 幂等决策树：不存在→NEW；PARSED 同哈希→RESUME；ARCHIVED 同哈希→DUPLICATE；异哈希→CONFLICT（不污染既有数据）
8. staging 写盘→解析→流式哈希→事务一→rename→事务二；rename 与最终对象同目录

### 验收
- [ ] `--parse` 跑通 testdata/dicom/ 全部 8+1 个文件（含冲突文件）
- [ ] 讲得出三个崩溃窗口及恢复路径
- [ ] 讲得出为什么事务里不做文件 IO

---

## 5. LruCache（建议 45 分钟）

**参考**：`04_/include/cache/LruCache.h`

### 需求
1. mutex + list<pair<K,V>> + unordered_map<K, list::iterator>
2. get 命中 splice 队头 O(1)；put 满淘汰队尾 O(1)；先删索引再删节点
3. 构造指定容量；size() 观测

### 验收
- [ ] 容量 3 依次 put a,b,c → get a → put d → b 应被淘汰
- [ ] 多线程 4×10000 次 put/get 无 crash（ThreadSanitizer 加分）

---

## 6. BK-tree 纠错（建议 90 分钟）

**参考**：`04_/src/spell/BkTree.cpp`

### 需求
1. 编辑距离：**码点维度**滚动数组 DP（utfcpp 切码点；非法 UTF-8 返回大距离）
2. insert：同词合并 freq；子树按与父词距离分桶（std::map<int,Node*>）
3. suggest：剪枝只访问边权 ∈ [d-max, d+max] 的子树；(distance, -freq) 排序取 TopK

### 验收
- [ ] "肺节结"→ 返回 [结节/肺/肺气肿]（距离 2）
- [ ] 能说清楚：为什么按字节算中文纠错会失效

---

## 7. TF-IDF 倒排索引与检索（建议 2 小时）

**参考**：`04_/src/index/InvertedIndex.cpp` + `src/index/Searcher.cpp`

### 需求（构建）
1. 解析目录全部 XML → report_id 去重 → 分词（conclusion×2 + description×1）
2. idf=log(N/df)；每文档 L2 归一化；倒排 term→[(doc,w)]
3. 版本目录落盘（meta/docs.tsv/postings.tsv，字段转义）；新版本 = max+1

### 需求（检索）
4. 查询分词同源 → TF×IDF → L2 归一化 → 点积累加 → 小顶堆 TopK
5. 摘要：命中词 ±24 字节，UTF-8 边界对齐（未命中路径也要对齐）
6. 快照不可变：atomic_load/store(shared_ptr)，每次检索取当前版本

### 验收
- [ ] --build-index 产出 v1/v2 共存目录；重启加载 max 版本
- [ ] "肺结节" 命中含磨玻璃结节的报告且排序合理
- [ ] 能讲：为什么文档向量构建期归一化（余弦=点积）

---

## 二档模块（理解即可，能 2 分钟裸讲）

| 模块 | 参考 | 2 分钟讲什么 |
|---|---|---|
| 索引构建主流程 | InvertedIndex.cpp build_index | 去重→分词→TF-IDF→归一化→版本目录 |
| 检索执行器 | Searcher.cpp | 查询向量→点积→TopK 堆→摘要边界 |
| SHA-256 流式 | 03_/src/util/Sha256.cpp | EVP+RAII+1MB 分块，内存与文件大小无关 |
| 四层入库 | ArchiveDao begin/finish | LAST_INSERT_ID 技巧、FOR UPDATE、两事务 |
| srpc 认证 | auth/main.cpp | PBKDF2、JWT 三段、RBAC 矩阵、Consul 注册 |
| Redis 封装 | RedisCache.cpp | 版本化键、FNV-1a、降级策略 |

## 三档（不用看）：main 脚手架、Config、Logger、CMake、docker-compose
