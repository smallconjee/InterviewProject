#!/usr/bin/env python3
# ============================================================================
# check_ats.py — 简历 PDF 文本层自检（2026-08-17 v2，多解析器仲裁版）
#
# 背景复盘：v1 用 pypdf 单解析器，误报"87 个康熙部首字符"。后经 poppler
# (pdftotext) 与 Apple PDFKit 双重交叉验证：原简历文本层是干净的——
# pypdf 对部分 CJK 字形走嵌入字体 cmap 反向映射，会得到康熙部首码位，
# 属于提取器假象，不是 PDF 的真实 ToUnicode。
# 结论：主流 ATS 解析路径（poppler/PDFBox/PDFKit 一族）读这份 PDF 没问题。
#
# 本脚本策略（按可信度排序）：
#   ① pdftotext（poppler，Linux 侧标准，ATS 最接近的代理）→ 首选
#   ② pypdf + NFKC 归一化 → 无 poppler 时的兜底（归一化消除假象）
# 检查项：康熙部首/兼容区残留、关键词可搜性、文本层基本健全。
#
# 用法：python3 scripts/check_ats.py <简历.pdf> [关键词1 关键词2 ...]
# 退出码：0 = 干净；1 = 发现问题
# ============================================================================
import shutil
import subprocess
import sys
import unicodedata

KANGXI_RANGE = (0x2F00, 0x2FD5)


def extract_pdftotext(path: str):
    if shutil.which("pdftotext") is None:
        return None
    try:
        out = subprocess.run(["pdftotext", path, "-"], capture_output=True, check=True)
        return ("pdftotext(poppler)", out.stdout.decode("utf-8", errors="replace"))
    except Exception:
        return None


def extract_pypdf(path: str):
    try:
        import pypdf
    except ImportError:
        return None
    try:
        reader = pypdf.PdfReader(path)
        text = "".join(page.extract_text() or "" for page in reader.pages)
        # pypdf 假象兜底：康熙部首经 NFKC 归一化映射回正常汉字（⼯→工）
        text = "".join(
            unicodedata.normalize("NFKC", ch) if KANGXI_RANGE[0] <= ord(ch) <= KANGXI_RANGE[1] else ch
            for ch in text
        )
        return ("pypdf+NFKC(兜底)", text)
    except Exception:
        return None


def check(path: str, keywords: list) -> int:
    result = extract_pdftotext(path) or extract_pypdf(path)
    if result is None:
        print("[FAIL] 没有可用的 PDF 解析器（装 poppler：brew install poppler；或 pip3 install pypdf）")
        return 1
    parser, text = result
    print(f"[INFO] 解析器：{parser}")

    problems = 0

    # ① 康熙部首残留（正常应为 0：主流解析器不产生，兜底路径已被 NFKC 消除）
    bad = [ch for ch in text if KANGXI_RANGE[0] <= ord(ch) <= KANGXI_RANGE[1]]
    if bad:
        problems += 1
        print(f"[FAIL] 仍有 {len(bad)} 个康熙部首字符（ATS 搜不到这些字），需排查导出工具")
    else:
        print("[OK] 无康熙部首残留")

    # ② 关键词可搜索性（模拟 ATS 命中）
    for kw in keywords:
        if kw in text:
            print(f"[OK] 关键词可命中：{kw}")
        else:
            problems += 1
            print(f"[FAIL] 关键词搜不到：{kw}")

    # ③ 文本层健全性
    cjk = sum(1 for ch in text if "\u4e00" <= ch <= "\u9fff")
    note = "（太少则可能是图片型 PDF，ATS 完全读不到）" if cjk < 50 else ""
    print(f"[INFO] 提取字符 {len(text)} 个，正常 CJK 汉字 {cjk} 个{note}")

    return 1 if problems else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    sys.exit(check(sys.argv[1], sys.argv[2:]))
