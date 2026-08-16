#!/bin/bash
# ============================================================================
# demo_ris.sh — RIS 检索系统一键演示（宿主机 Mac 运行）
# 前置：容器内已 build；testdata/ris/reports 已生成；服务未占用 9090
# ============================================================================
set -e
cd "$(dirname "$0")/.."

RIS_HOST="${RIS_HOST:-medical-pacs-dev-dev-1.orb.local}"

echo "════ 1. 生成测试报告 + 离线构建索引（若已有则跳过）════"
[ -d testdata/ris/reports ] || python3 scripts/gen_reports.py
docker exec medical-pacs-dev-dev-1 bash -c '
cd /workspace
[ -d data/ris/index ] || RIS_INDEX_DIR=data/ris/index ./build/bin/ris_report_search --build-index testdata/ris/reports
pkill -f "bin/ris_report_search$" 2>/dev/null || true
sleep 0.5
RIS_INDEX_DIR=data/ris/index stdbuf -oL ./build/bin/ris_report_search > /tmp/ris.log 2>&1 &
sleep 3
grep -E "索引加载|BK-tree|监听" /tmp/ris.log'

echo "════ 2. TLV 协议全套测试（半帧/多帧/非法帧/纠错/缓存）════"
RIS_HOST=$RIS_HOST python3 scripts/test_ris_client.py

echo "════ 3. 报告-影像联动（张三的报告关联 PACS 归档）════"
python3 - << EOF
import socket, struct, json
def encode(tp, p): return struct.pack('>BBI', tp, 0, len(p)) + p
addr = socket.getaddrinfo('$RIS_HOST', 9090, socket.AF_INET, socket.SOCK_STREAM)[0][4]
s = socket.create_connection(addr, timeout=5)
s.sendall(encode(0x01, '磨玻璃'.encode()))
head = b''
while len(head) < 6: head += s.recv(6-len(head))
tp, fl, ln = struct.unpack('>BBI', head)
body = b''
while len(body) < ln: body += s.recv(ln-len(body))
r = json.loads(body)
for h in r['hits'][:3]:
    print(f"  {h['report_id']} {h['patient']} score={h['score']:.3f} has_image={h['has_image']}")
EOF

echo "════ 4. 索引版本切换 + 缓存版本化键 ════"
docker exec medical-pacs-dev-dev-1 bash -c 'cd /workspace && RIS_INDEX_DIR=data/ris/index ./build/bin/ris_report_search --build-index testdata/ris/reports'
echo "  新版本已生成（v+1）；服务日志中的 L1/L2 命中："
docker exec medical-pacs-dev-dev-1 bash -c 'grep -E "命中" /tmp/ris.log | tail -3'
echo "  Redis 版本化键（v1/v2 共存，旧键靠 TTL 回收）："
docker exec medical-pacs-dev-redis-1 redis-cli --scan --pattern 'ris:v*' | sort | head -4

echo
echo "演示完成。服务日志: docker exec medical-pacs-dev-dev-1 tail -f /tmp/ris.log"
