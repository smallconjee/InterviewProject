#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_dicom.py — 生成 DICOM Part-10 测试文件（在 Mac 宿主机运行，写入 testdata/dicom/）

覆盖场景：
  01_ok_explicit        显式 VR 小端，完整四层元数据（主路径）
  02_ok_implicit        隐式 VR 小端（默认传输语法）
  03_ok_jpeg_ts         JPEG 压缩传输语法（数据集仍为显式小端）
  04_ok_sq_nested       含未定长嵌套 SQ 的文件（验证序列跳过、内部 tag 不泄漏）
  05_truncated          截断文件（模拟导入中断）
  06_not_dicom          非 DICOM 文件（魔数错误）
  07_bigendian          Big Endian 传输语法（应被拒绝）
  08_missing_required   缺 SeriesInstanceUID（应报 MISSING_REQUIRED）
"""
import os
import struct

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "testdata", "dicom")

TS_IMPLICIT = "1.2.840.10008.1.2"
TS_EXPLICIT = "1.2.840.10008.1.2.1"
TS_JPEG = "1.2.840.10008.1.2.4.50"
TS_BIGENDIAN = "1.2.840.10008.1.2.2"

LONG_VRS = {b"OB", b"OW", b"OF", b"OD", b"OL", b"SQ", b"UC", b"UR", b"UT", b"UN"}


def pad_value(vr: bytes, value: bytes) -> bytes:
    """DICOM 要求值长度为偶数：UI 用 \\0 补，其余用空格补"""
    if len(value) % 2 == 0:
        return value
    return value + (b"\x00" if vr == b"UI" else b" ")


def elem(group: int, element: int, vr: bytes, value: bytes, explicit: bool) -> bytes:
    value = pad_value(vr, value)
    tag = struct.pack("<HH", group, element)
    if not explicit:
        # 隐式 VR：tag + u32 长度 + 值
        return tag + struct.pack("<I", len(value)) + value
    if vr in LONG_VRS:
        # 长形式：VR + 2 保留字节 + u32 长度
        return tag + vr + b"\x00\x00" + struct.pack("<I", len(value)) + value
    # 短形式：VR + u16 长度
    return tag + vr + struct.pack("<H", len(value)) + value


def item(content: bytes, undefined: bool = False) -> bytes:
    """数据项 (FFFE,E000)：u32 长度、无 VR；undefined=True 表示未定长（由内容+结束符组成）"""
    if undefined:
        return struct.pack("<HH", 0xFFFE, 0xE000) + struct.pack("<I", 0xFFFFFFFF) + content + \
               struct.pack("<HH", 0xFFFE, 0xE00D) + struct.pack("<I", 0)
    return struct.pack("<HH", 0xFFFE, 0xE000) + struct.pack("<I", len(content)) + content


def sq_undefined(inner: bytes) -> bytes:
    """未定长 SQ 元素：tag + SQ + 保留 + 0xFFFFFFFF + items + (FFFE,E0DD)"""
    return (struct.pack("<HH", 0x0008, 0x1140) + b"SQ" + b"\x00\x00" +
            struct.pack("<I", 0xFFFFFFFF) + inner +
            struct.pack("<HH", 0xFFFE, 0xE0DD) + struct.pack("<I", 0))


def file_meta(ts: str, sop_class: str, sop_iuid: str) -> bytes:
    """文件元组（恒为显式 VR 小端）；(0002,0000) 组长度 = 其后元组元素总字节数"""
    body = (elem(0x0002, 0x0001, b"OB", b"\x00\x01", True) +
            elem(0x0002, 0x0002, b"UI", sop_class.encode(), True) +
            elem(0x0002, 0x0003, b"UI", sop_iuid.encode(), True) +
            elem(0x0002, 0x0010, b"UI", ts.encode(), True))
    return elem(0x0002, 0x0000, b"UL", struct.pack("<I", len(body)), True) + body


def base_dataset(explicit: bool, sop_iuid: str, **kw) -> bytes:
    """按 tag 升序构造数据集（Part-10 要求）；kw 支持覆盖默认值/删除字段（值传 None）"""
    vals = {
        "sop_class": "1.2.840.10008.5.1.4.1.1.2",  # CT Image Storage
        "sop_iuid": sop_iuid,
        "study_date": kw.get("study_date", "20260801"),
        "accession": kw.get("accession", "ACC-2026-0001"),
        "modality": kw.get("modality", "CT"),
        "study_desc": kw.get("study_desc", "胸部平扫"),
        "patient_name": kw.get("patient_name", "张三"),
        "patient_id": kw.get("patient_id", "P001"),
        "issuer": kw.get("issuer", "HOSP-A"),
        "birth_date": kw.get("birth_date", "19900101"),
        "sex": kw.get("sex", "M"),
        "study_iuid": kw.get("study_iuid", "1.2.826.0.1.3680043.2.1143.1"),
        "series_iuid": kw.get("series_iuid", "1.2.826.0.1.3680043.2.1143.2"),
        "series_number": kw.get("series_number", "1"),
        "instance_number": kw.get("instance_number", "1"),
        "pixel": kw.get("pixel", bytes(range(64))),  # 64 字节假像素数据
    }
    def enc(v):
        return v if isinstance(v, bytes) else v.encode("utf-8")

    e = lambda g, i, vr, v: (b"" if v is None else elem(g, i, vr, enc(v), explicit))
    ds = b""
    ds += e(0x0008, 0x0016, b"UI", vals["sop_class"])
    ds += e(0x0008, 0x0018, b"UI", vals["sop_iuid"])
    ds += e(0x0008, 0x0020, b"DA", vals["study_date"])
    ds += e(0x0008, 0x0050, b"SH", vals["accession"])
    ds += e(0x0008, 0x0060, b"CS", vals["modality"])
    ds += e(0x0008, 0x1030, b"LO", vals["study_desc"])
    ds += e(0x0008, 0x103E, b"LO", "轴位")
    ds += e(0x0010, 0x0010, b"PN", vals["patient_name"])
    ds += e(0x0010, 0x0020, b"LO", vals["patient_id"])
    ds += e(0x0010, 0x0021, b"LO", vals["issuer"])
    ds += e(0x0010, 0x0030, b"DA", vals["birth_date"])
    ds += e(0x0010, 0x0040, b"CS", vals["sex"])
    ds += e(0x0020, 0x000D, b"UI", vals["study_iuid"])
    ds += e(0x0020, 0x000E, b"UI", vals["series_iuid"])
    ds += e(0x0020, 0x0011, b"IS", vals["series_number"])
    ds += e(0x0020, 0x0013, b"IS", vals["instance_number"])
    ds += e(0x7FE0, 0x0010, b"OW", vals["pixel"])
    return ds


def make_file(ts: str, dataset: bytes, sop_class="1.2.840.10008.5.1.4.1.1.2",
              sop_iuid="1.2.826.0.1.3680043.2.1143.100") -> bytes:
    return b"\x00" * 128 + b"DICM" + file_meta(ts, sop_class, sop_iuid) + dataset


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    files = {}

    # 1) 主路径：显式 VR 小端
    files["01_ok_explicit.dcm"] = make_file(
        TS_EXPLICIT, base_dataset(True, "1.2.826.0.1.3680043.2.1143.101"))

    # 2) 隐式 VR 小端
    files["02_ok_implicit.dcm"] = make_file(
        TS_IMPLICIT, base_dataset(False, "1.2.826.0.1.3680043.2.1143.102"))

    # 3) JPEG 传输语法：数据集仍显式小端，像素区不解析
    files["03_ok_jpeg_ts.dcm"] = make_file(
        TS_JPEG, base_dataset(True, "1.2.826.0.1.3680043.2.1143.103"))

    # 4) 未定长嵌套 SQ（内部埋一个假 (0010,0010)，验证不泄漏到顶层）
    fake_in_seq = elem(0x0010, 0x0010, b"PN", b"WRONG-NAME", True)
    nested = sq_undefined(item(fake_in_seq, undefined=True))
    outer = sq_undefined(item(nested, undefined=True))
    ds4 = base_dataset(True, "1.2.826.0.1.3680043.2.1143.104")
    # 把 SQ 插到 0008 组之后、0010 组之前（保持 tag 升序：0x00081140）
    anchor = ds4.index(elem(0x0010, 0x0010, b"PN", "张三".encode("utf-8"), True))
    files["04_ok_sq_nested.dcm"] = make_file(TS_EXPLICIT, ds4[:anchor] + outer + ds4[anchor:])

    # 5) 截断：取 01 号文件的前 60%（切在数据集中间）
    ok = make_file(TS_EXPLICIT, base_dataset(True, "1.2.826.0.1.3680043.2.1143.101"))
    files["05_truncated.dcm"] = ok[: int(len(ok) * 0.6)]

    # 6) 非 DICOM：随机内容
    files["06_not_dicom.bin"] = b"THIS IS NOT A DICOM FILE" + b"\x55" * 200

    # 7) Big Endian 传输语法（元组本身仍小端；数据集按 BE 编码——解析器应在 TS 处拒绝）
    files["07_bigendian.dcm"] = make_file(TS_BIGENDIAN, b"")

    # 8) 缺 SeriesInstanceUID
    ds8 = base_dataset(True, "1.2.826.0.1.3680043.2.1143.108", series_iuid=None)
    files["08_missing_required.dcm"] = make_file(
        TS_EXPLICIT, ds8, sop_iuid="1.2.826.0.1.3680043.2.1143.108")

    for name, content in files.items():
        path = os.path.join(OUT_DIR, name)
        with open(path, "wb") as f:
            f.write(content)
        print(f"生成 {path} ({len(content)} 字节)")


if __name__ == "__main__":
    main()
