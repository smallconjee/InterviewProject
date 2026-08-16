#!/bin/bash
# 初始化/重置 pacs_db 表结构（开发环境用）
# ⚠️ 会 DROP 重建全部表；生产环境应使用版本化迁移工具，不要直接跑本脚本
set -e

cd "$(dirname "$0")/.."

MYSQL_CONTAINER="${MYSQL_CONTAINER:-medical-pacs-dev-mysql-1}"

docker exec -i "$MYSQL_CONTAINER" mysql -uroot -proot < sql/pacs_db_init.sql
echo "=== pacs_db 表结构初始化完成（patient/study/series/sop_instance/backup_event）==="
