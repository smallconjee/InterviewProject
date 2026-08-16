#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_ris_client.py — RIS 检索服务 TLV 协议测试客户端（宿主机运行）

测试覆盖：
  1. 正常检索（应返回 JSON 结果）
  2. 纠错建议
  3. 半帧：头部与体拆成两次 send
  4. 连续多帧：一次 send 两条请求
  5. 非法类型：应收到 ERROR 帧并被服务端断开
  6. 二次同查询：应命中缓存（服务端日志可见，客户端验证结果一致性）
"""
import json
import socket
import struct
import sys

import os
HOST = os.environ.get("RIS_HOST", "medical-pacs-dev-dev-1.orb.local")
PORT = int(os.environ.get("RIS_PORT", "9090"))



def connect(timeout=5):
    """强制 IPv4：orb.local 会同时解析出 v6，容器网络仅 v4 可达"""
    addr = socket.getaddrinfo(HOST, PORT, socket.AF_INET, socket.SOCK_STREAM)[0][4]
    return socket.create_connection(addr, timeout=timeout)

def encode(tp: int, payload: bytes) -> bytes:
    return struct.pack(">BBI", tp, 0, len(payload)) + payload


def recv_frame(sock) -> tuple:
    head = b""
    while len(head) < 6:
        chunk = sock.recv(6 - len(head))
        if not chunk:
            raise ConnectionError("连接被关闭")
        head += chunk
    tp, flags, length = struct.unpack(">BBI", head)
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            raise ConnectionError("连接被关闭（体未收完）")
        body += chunk
    return tp, body


def test_normal():
    sock = connect()
    sock.sendall(encode(0x01, "肺结节".encode()))
    tp, body = recv_frame(sock)
    result = json.loads(body)
    print(f"[1] 正常检索 '肺结节': {tp:#x} 命中 {result['count']} 条, "
          f"首条 report={result['hits'][0]['report_id'] if result['hits'] else '-'}")
    assert tp == 0x11
    sock.close()
    return result


def test_suggest():
    sock = connect()
    sock.sendall(encode(0x02, "肺节结".encode()))  # 故意颠倒一个字
    tp, body = recv_frame(sock)
    result = json.loads(body)
    print(f"[2] 纠错建议 '肺节结': {result['suggestions'][:3]}")
    assert tp == 0x12
    sock.close()


def test_partial_frame():
    sock = connect()
    frame = encode(0x01, "骨折".encode())
    sock.sendall(frame[:4])   # 半个头
    sock.sendall(frame[4:9])  # 剩余头 + 半个体
    sock.sendall(frame[9:])   # 剩余体
    tp, body = recv_frame(sock)
    result = json.loads(body)
    print(f"[3] 半帧三段发送 '骨折': {tp:#x} 命中 {result['count']} 条")
    assert tp == 0x11
    sock.close()


def test_multi_frame():
    sock = connect()
    # 一次 send 两条完整帧
    payload = encode(0x01, "囊肿".encode()) + encode(0x01, "脑梗死".encode())
    sock.sendall(payload)
    tp1, body1 = recv_frame(sock)
    tp2, body2 = recv_frame(sock)
    r1, r2 = json.loads(body1), json.loads(body2)
    print(f"[4] 连续多帧: 囊肿命中 {r1['count']}, 脑梗死命中 {r2['count']}")
    assert tp1 == 0x11 and tp2 == 0x11
    sock.close()


def test_bad_type():
    sock = connect()
    sock.sendall(encode(0x99, b"evil"))
    tp, body = recv_frame(sock)
    print(f"[5] 非法类型 0x99: 收到 {tp:#x} {body.decode()}")
    assert tp == 0x7F
    # 服务端应主动断开
    rest = sock.recv(1024)
    assert rest == b"", f"应无更多数据，收到 {rest}"
    print("    服务端已按预期断开连接")
    sock.close()


def test_cache_consistency(first_result):
    sock = connect()
    sock.sendall(encode(0x01, "肺结节".encode()))
    tp, body = recv_frame(sock)
    result = json.loads(body)
    same = result["hits"][0]["report_id"] == first_result["hits"][0]["report_id"]
    print(f"[6] 二次查询结果一致(缓存路径): {same} count={result['count']}")
    assert same
    sock.close()


def main():
    r = test_normal()
    test_suggest()
    test_partial_frame()
    test_multi_frame()
    test_bad_type()
    test_cache_consistency(r)
    print("\n全部 6 项 TLV 协议测试通过 ✅")


if __name__ == "__main__":
    main()
