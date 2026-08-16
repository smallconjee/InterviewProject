-- ============================================================================
-- pacs_db_init.sql — PACS 影像归档库表结构（开发环境初始化/重置脚本）
--
-- 用法（宿主机或容器内）：
--   bash scripts/init_db.sh
--   或 docker exec -i medical-pacs-dev-mysql-1 mysql -uroot -proot < sql/pacs_db_init.sql
--
-- ⚠️ 本脚本会 DROP 重建全部表，仅供开发环境重置；生产环境应使用版本化
--    迁移工具（Flyway/Liquibase），演进点见 05_docs 待补清单。
--
-- 数据模型：DICOM 天然四层层级
--   patient(患者) 1..N study(检查) 1..N series(序列) 1..N sop_instance(实例)
--   层间用自增代理键 id 关联（JOIN 用），各层 DICOM UID 建唯一键（业务幂等用）
--
-- 实例状态机（续传的基础，阶段 1 实现）：
--   RECEIVED(已落盘未解析) → PARSED(解析+校验完成) → ARCHIVED(归档事务提交)
--   终态：DUPLICATE(同UID同哈希重复导入) / CONFLICT(同UID异哈希) / FAILED
--   中断恢复：处于 RECEIVED/PARSED 的未完成对象，再次导入时按续传处理而非拒绝
--
-- 备份状态（阶段 2 消费端异步更新）：
--   NONE → PENDING(备份事件已入本地消息表) → BACKED_UP / FAILED
-- ============================================================================

USE pacs_db;

SET NAMES utf8mb4;

-- 逆序删除，避免外键约束阻塞
DROP TABLE IF EXISTS backup_event;
DROP TABLE IF EXISTS sop_instance;
DROP TABLE IF EXISTS series;
DROP TABLE IF EXISTS study;
DROP TABLE IF EXISTS patient;
DROP TABLE IF EXISTS user_account;

-- ----------------------------------------------------------------------------
-- patient — 患者层
-- 身份 = (PatientID, IssuerOfPatientID) 复合唯一：
--   同一 PatientID 来自不同发行方（不同医院/系统）= 不同患者（简历 bullet 2）
-- issuer 用 NOT NULL DEFAULT '' 而非 NULL：MySQL 唯一索引对 NULL 不生效
--   （多个 NULL 可同时存在），空串才能参与唯一约束——真实会踩的坑
-- ----------------------------------------------------------------------------
CREATE TABLE patient (
    id           BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    patient_id   VARCHAR(64)  NOT NULL COMMENT 'DICOM (0010,0020) PatientID',
    issuer       VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'DICOM (0010,0021) IssuerOfPatientID，患者来源',
    patient_name VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'DICOM (0010,0010)，utf8mb4 支持中文',
    birth_date   DATE         NULL COMMENT 'DICOM (0010,0030)',
    sex          CHAR(1)      NULL COMMENT 'DICOM (0010,0040) M/F/O',
    created_at   DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3) COMMENT '毫秒精度，排障用',
    updated_at   DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                             ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE KEY uk_patient (patient_id, issuer),
    CONSTRAINT chk_sex CHECK (sex IN ('M', 'F', 'O'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='患者层：身份按 (PatientID, Issuer) 区分';

-- ----------------------------------------------------------------------------
-- study — 检查层（一次成像检查）
-- StudyInstanceUID 全局唯一（DICOM 标准保证），是报告检索侧关联影像的锚点
-- 查询路径：按患者查历史检查（idx_patient），故 patient_id 上建普通索引
-- ----------------------------------------------------------------------------
CREATE TABLE study (
    id                 BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    patient_fk         BIGINT UNSIGNED NOT NULL COMMENT '所属患者 patient.id',
    study_instance_uid VARCHAR(64)  NOT NULL COMMENT 'DICOM (0020,000D)，业务唯一键',
    study_date         DATE         NULL COMMENT 'DICOM (0008,0020)',
    accession_number   VARCHAR(64)  NOT NULL DEFAULT '' COMMENT 'DICOM (0008,0050) 检查号，RIS 侧常用检索键',
    study_description  VARCHAR(256) NOT NULL DEFAULT '' COMMENT 'DICOM (0008,1030)',
    created_at         DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at         DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                   ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE KEY uk_study_uid (study_instance_uid),
    KEY idx_patient (patient_fk),
    CONSTRAINT fk_study_patient FOREIGN KEY (patient_fk) REFERENCES patient (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='检查层：StudyInstanceUID 唯一';

-- ----------------------------------------------------------------------------
-- series — 序列层（一次检查内的同参数成像序列，如 T1/T2 加权）
-- modality 冗余存储在序列层：DICOM 中 (0008,0060) 是序列级属性，
--   检查级"检查有哪些模态"由 series 聚合得到，不冗余存 study（避免更新不一致）
-- ----------------------------------------------------------------------------
CREATE TABLE series (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    study_fk            BIGINT UNSIGNED NOT NULL COMMENT '所属检查 study.id',
    series_instance_uid VARCHAR(64)  NOT NULL COMMENT 'DICOM (0020,000E)，业务唯一键',
    modality            VARCHAR(16)  NOT NULL DEFAULT '' COMMENT 'DICOM (0008,0060) CT/MR/DR...',
    series_number       INT          NULL COMMENT 'DICOM (0020,0011)',
    series_description  VARCHAR(256) NOT NULL DEFAULT '' COMMENT 'DICOM (0008,103E)',
    created_at          DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at          DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                    ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE KEY uk_series_uid (series_instance_uid),
    KEY idx_study (study_fk),
    CONSTRAINT fk_series_study FOREIGN KEY (study_fk) REFERENCES study (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='序列层：SeriesInstanceUID 唯一';

-- ----------------------------------------------------------------------------
-- sop_instance — 实例层（一个 DICOM 文件 = 一个 SOP Instance）
-- ★ uk_sop_uid 唯一索引是整个幂等设计的地基：
--   并发重复导入时第二个 INSERT 报 1062，转为"重查后按哈希决策"，
--   避免"先查后插"的竞态窗口（两个线程都查不到、都插入）
-- sha256 固定 64 位十六进制 → CHAR(64) 比 VARCHAR 省一字节长度前缀且语义更准
-- storage_path 存相对路径：迁移存储介质/换挂载点时不需要刷库
-- ----------------------------------------------------------------------------
CREATE TABLE sop_instance (
    id                BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    series_fk         BIGINT UNSIGNED NOT NULL COMMENT '所属序列 series.id',
    sop_instance_uid  VARCHAR(64)  NOT NULL COMMENT 'DICOM (0008,0018)，幂等键',
    sop_class_uid     VARCHAR(64)  NOT NULL DEFAULT '' COMMENT 'DICOM (0008,0016)，CT/MR 存储类等',
    instance_number   INT          NULL COMMENT 'DICOM (0020,0013)',
    sha256            CHAR(64)     NULL COMMENT '文件内容哈希（解析阶段计算后回填）',
    file_size         BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '字节数',
    storage_path      VARCHAR(512) NOT NULL DEFAULT '' COMMENT '归档相对路径',
    status            VARCHAR(16)  NOT NULL DEFAULT 'RECEIVED' COMMENT '归档状态机',
    backup_status     VARCHAR(16)  NOT NULL DEFAULT 'NONE' COMMENT '备份状态（阶段 2 启用）',
    created_at        DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at        DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                   ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE KEY uk_sop_uid (sop_instance_uid),
    KEY idx_series (series_fk),
    KEY idx_backup_scan (backup_status, status) COMMENT '备份补偿扫描的查询路径',
    CONSTRAINT fk_instance_series FOREIGN KEY (series_fk) REFERENCES series (id),
    CONSTRAINT chk_status CHECK (status IN
        ('RECEIVED', 'PARSED', 'ARCHIVED', 'DUPLICATE', 'CONFLICT', 'FAILED')),
    CONSTRAINT chk_backup CHECK (backup_status IN
        ('NONE', 'PENDING', 'BACKED_UP', 'FAILED'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='实例层：SOPInstanceUID 唯一，幂等与状态机所在';

-- ----------------------------------------------------------------------------
-- backup_event — 本地消息表（阶段 2 启用，随归档事务同库同事务写入）
-- 目标：与 sop_instance 的状态变更在【同一个本地事务】里提交，
--   要么都成功要么都回滚——这是"消息不丢"的第一道防线（简历 bullet 4）
-- task_id 唯一：消费端据此去重（重复投递只生效一次）
-- idx_status_retry 支撑定时补偿扫描：WHERE status IN (...) AND next_retry_at <= now
-- ----------------------------------------------------------------------------
CREATE TABLE backup_event (
    id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    task_id       CHAR(36)     NOT NULL COMMENT 'UUID，消费端幂等键',
    instance_fk   BIGINT UNSIGNED NOT NULL COMMENT '待备份实例 sop_instance.id',
    status        VARCHAR(16)  NOT NULL DEFAULT 'PENDING' COMMENT 'PENDING/PUBLISHED/CONFIRMED/DEAD',
    retry_count   INT          NOT NULL DEFAULT 0 COMMENT '有界重试计数',
    next_retry_at DATETIME(3)  NULL COMMENT '下次可投递时间（指数退避）',
    created_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                ON UPDATE CURRENT_TIMESTAMP(3),
    UNIQUE KEY uk_task_id (task_id),
    KEY idx_status_retry (status, next_retry_at),
    CONSTRAINT fk_event_instance FOREIGN KEY (instance_fk) REFERENCES sop_instance (id),
    CONSTRAINT chk_event_status CHECK (status IN
        ('PENDING', 'PUBLISHED', 'CONFIRMED', 'DEAD'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='本地消息表：与归档状态同事务提交';

-- ----------------------------------------------------------------------------
-- user_account — 认证服务用户表（阶段 3）
-- 密码存 PBKDF2-HMAC-SHA256(100000 迭代, 16B 随机盐, 32B 输出) 的十六进制
-- 种子用户由 scripts/seed_users.sh 写入（admin/admin123、doctor/doctor123）
-- ----------------------------------------------------------------------------
CREATE TABLE user_account (
    id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    username      VARCHAR(64) NOT NULL,
    password_hash CHAR(64)    NOT NULL COMMENT 'PBKDF2 输出 32B 的十六进制',
    salt          CHAR(32)    NOT NULL COMMENT '随机盐 16B 的十六进制',
    role          VARCHAR(16) NOT NULL DEFAULT 'radiologist' COMMENT 'radiologist / admin',
    created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UNIQUE KEY uk_username (username),
    CONSTRAINT chk_role CHECK (role IN ('radiologist', 'admin'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='认证用户';
