#!/bin/bash
# ============================================================================
# demo_pacs.sh — PACS 归档系统一键演示（容器内运行）
# 覆盖：登录→鉴权→导入(新/重复/冲突)→查询→消息表状态→备份队列
# 前置：bash scripts/init_db.sh && bash scripts/seed_users.sh 已执行
# ============================================================================
set -e
cd "$(dirname "$0")/.."
BIN=./build/bin/pacs_archive_service
TESTDATA=testdata/dicom

echo "════ 1. 启动认证服务 + 归档服务 ════"
./build/bin/pacs_auth_service > /tmp/demo_auth.log 2>&1 & AUTH_PID=$!
sleep 1
$BIN > /tmp/demo_pacs.log 2>&1 & PACS_PID=$!
sleep 1
trap "kill $AUTH_PID $PACS_PID 2>/dev/null" EXIT

echo "════ 2. doctor 登录拿 token（srpc + PBKDF2 + JWT）════"
TOKEN=$($BIN --login doctor doctor123 2>/dev/null)
echo "token: ${TOKEN:0:50}..."

echo "════ 3. 无 token 访问 → 401 ════"
curl -s -o /dev/null -w "  [%{http_code}]\n" http://127.0.0.1:8080/api/v1/studies

echo "════ 4. doctor 访问管理接口 → 403（RBAC）════"
curl -s -o /dev/null -w "  [%{http_code}]\n" -H "Authorization: Bearer $TOKEN" \
    http://127.0.0.1:8080/api/v1/admin/backup-status

echo "════ 5. 导入新影像 → 201（本地消息表同事务 + confirm 发布）════"
curl -s -w "\n  [%{http_code}]\n" --data-binary @$TESTDATA/01_ok_explicit.dcm \
    -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/images/import

echo "════ 6. 重复导入（同 UID 同哈希）→ 200 幂等 ════"
curl -s -w "\n  [%{http_code}]\n" --data-binary @$TESTDATA/01_ok_explicit.dcm \
    -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/images/import

echo "════ 7. 冲突导入（同 UID 异哈希）→ 409 拒绝 ════"
curl -s -w "\n  [%{http_code}]\n" --data-binary @$TESTDATA/09_conflict.dcm \
    -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/v1/images/import

echo "════ 8. 检查列表（张三的检查，中文 utf8mb4）════"
curl -s -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:8080/api/v1/studies?patient_id=P001"; echo

echo "════ 9. admin 看本地消息表状态（PUBLISHED = broker confirm 成功）════"
ATOKEN=$($BIN --login admin admin123 2>/dev/null)
curl -s -H "Authorization: Bearer $ATOKEN" http://127.0.0.1:8080/api/v1/admin/backup-status; echo

echo "════ 10. RabbitMQ 队列深度（消息已入队等消费）════"
docker exec medical-pacs-dev-rabbitmq-1 rabbitmqctl list_queues name messages 2>/dev/null | grep pacs || true

echo
echo "演示完成。日志：/tmp/demo_pacs.log /tmp/demo_auth.log"
echo "OSS 消费端演示：export PACS_OSS_* 四个环境变量后运行 ./build/bin/pacs_backup_consumer"
