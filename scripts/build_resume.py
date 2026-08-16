#!/usr/bin/env python3
# ============================================================================
# build_resume.py — 简历一键构建 v2（2026-08-17：样式可调 + 模块自由增删）
#
# 做什么：读取 05_docs/简历/master/简历内容.md（唯一需要编辑的正本），
#   套样式生成 HTML，用 Edge/Chrome headless 打成 PDF，最后自动跑 ATS 自检。
#
# 用法：python3 scripts/build_resume.py
#   产物：05_docs/简历/master/周航锐_C++_简历_最新.pdf（+ 过程文件 _resume.html）
#
# 正本格式约定（解析严格，格式错了报行号）：
#   内容结构：
#     @name 姓名              @title 职位 ｜ 经验      @contact ☎ 手机 ✉ 邮箱
#     @section 板块标题       ← 模块随便加：新起一段 @section + 条目即可；删模块=整段删
#     @project 项目名 | 日期  @job 公司 ｜ 职位 | 日期  @edu 学校 ｜ 专业 | 日期
#     @desc 简介行            @stack 技术栈行          @text 普通段落（如自我评价的散文）
#     - 列表条目（**加粗** 支持）
#   样式调节（写在文件任意位置，一般放头部；不写就用默认值）：
#     @样式 键 值             ← 如：@样式 行距 1.6
#   <!-- --> 注释与空行忽略
# ============================================================================
import html
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MASTER = ROOT / "05_docs/简历/master/简历内容.md"
OUT_DIR = MASTER.parent
OUT_HTML = OUT_DIR / "_resume.html"
OUT_PDF = OUT_DIR / "周航锐_C++_简历_最新.pdf"
BROWSER_CANDIDATES = [
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
]

# 样式旋钮：键 → (默认值, 最小值, 最大值)。CSS 单位见 TEMPLATE 里的注释。
STYLE_SCHEMA = {
    "正文字号":   (9.2, 6.0, 14.0),   # px，条目/段落正文
    "姓名字号":   (22, 14, 32),       # px
    "职位字号":   (11, 8, 16),        # px，姓名下一行
    "标题字号":   (12, 9, 18),        # px，浅蓝条板块标题
    "日期字号":   (9, 7, 13),         # px，右侧日期
    "行距":       (1.52, 1.1, 2.2),   # 倍数
    "页边距上":   (11, 0, 25),        # mm
    "页边距下":   (9, 0, 25),         # mm
    "页边距左":   (13, 5, 25),        # mm
    "页边距右":   (13, 5, 25),        # mm
    "板块间距":   (9, 3, 20),         # px，板块标题与上一板块的距离
    "条目间距":   (2.5, 0, 10),       # px，相邻列表条目的距离
}

TEMPLATE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>简历</title>
<style>
  @page {{ size: A4; margin: 0; }}
  * {{ margin: 0; padding: 0; box-sizing: border-box; }}
  body {{ font-family: "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", sans-serif;
         color: #2d3436; font-size: {s[正文字号]}px; line-height: {s[行距]};
         -webkit-print-color-adjust: exact; print-color-adjust: exact; }}
  .sheet {{ width: 210mm; min-height: 297mm;
            padding: {s[页边距上]}mm {s[页边距右]}mm {s[页边距下]}mm {s[页边距左]}mm; }}
  .header {{ text-align: center; margin-bottom: 7px; }}
  .name {{ font-size: {s[姓名字号]}px; font-weight: 700; letter-spacing: 2px; }}
  .title-line {{ font-size: {s[职位字号]}px; color: #636e72; margin-top: 2px; }}
  .contact {{ font-size: {s[正文字号]}px; color: #636e72; margin-top: 3px; }}
  .contact .ico {{ color: #2e86c1; font-style: normal; margin: 0 3px 0 12px; }}
  .contact .ico:first-child {{ margin-left: 0; }}
  .section-title {{ background: #e8f3fb; color: #1f6fb2; font-size: {s[标题字号]}px;
                    font-weight: 700; padding: 3px 10px; margin: {s[板块间距]}px 0 5px;
                    border-radius: 2px; }}
  .section-title.first {{ margin-top: 0; }}
  ul {{ list-style: none; }}
  li {{ position: relative; padding-left: 11px; margin-bottom: {s[条目间距]}px; }}
  li::before {{ content: "•"; color: #2e86c1; position: absolute; left: 1px; }}
  .item-head {{ margin-bottom: 2px; }}
  .item-head .iname {{ font-size: {s[标题字号]}px; font-weight: 700; display: inline; }}
  .item-head .idate {{ float: right; color: #636e72; font-size: {s[日期字号]}px; padding-top: 2px; }}
  .item-desc {{ margin-bottom: 1.5px; text-align: justify; }}
  .item-stack {{ color: #636e72; margin-bottom: {s[条目间距]}px; }}
  .item-stack b {{ color: #2d3436; font-weight: 600; }}
  .strong {{ font-weight: 600; }}
</style>
</head>
<body>
<div class="sheet">
{HEADER}
{BODY}
</div>
</body>
</html>
"""


def die(msg: str, lineno: int = None):
    where = f"（第 {lineno} 行）" if lineno else ""
    print(f"[构建失败]{where}: {msg}")
    sys.exit(1)


def inline(text: str) -> str:
    """行内转换：HTML 转义 + **加粗**。"""
    text = html.escape(text, quote=False)
    return re.sub(r"\*\*(.+?)\*\*", r'<span class="strong">\1</span>', text)


def contact_html(raw: str) -> str:
    parts = []
    for tok in raw.split():
        if tok in ("☎", "✉", "📞", "📧"):
            parts.append(f'<i class="ico">{tok}</i>')
        else:
            parts.append(tok)
    return "".join(parts)


def parse_and_render(md_text: str) -> str:
    md_text = re.sub(r"<!--.*?-->", "", md_text, flags=re.S)
    lines = md_text.splitlines()

    style = {k: v[0] for k, v in STYLE_SCHEMA.items()}
    header = {"name": "", "title": "", "contact": ""}
    body, cur_list = [], []
    section_count = 0

    def flush_list():
        nonlocal cur_list
        if cur_list:
            body.append("<ul>" + "".join(cur_list) + "</ul>")
            cur_list = []

    for i, raw in enumerate(lines, 1):
        ln = raw.strip()
        if not ln:
            continue
        if ln.startswith("@"):
            flush_list()
            tag, _, val = ln.partition(" ")
            val = val.strip()
            if tag == "@name":
                header["name"] = inline(val)
            elif tag == "@title":
                header["title"] = inline(val)
            elif tag == "@contact":
                header["contact"] = contact_html(val)
            elif tag == "@样式":
                parts = val.split()
                if len(parts) != 2:
                    die('@样式 行格式：@样式 键 值（如 @样式 行距 1.6）', i)
                key, sval = parts
                if key not in STYLE_SCHEMA:
                    die(f'未知样式键「{key}」，可用：{"、".join(STYLE_SCHEMA)}', i)
                try:
                    num = float(sval)
                except ValueError:
                    die(f'样式值必须是数字：{key} {sval}', i)
                _, lo, hi = STYLE_SCHEMA[key]
                if not (lo <= num <= hi):
                    die(f'样式值超范围：{key} 允许 {lo}–{hi}，收到 {sval}', i)
                style[key] = num
            elif tag == "@section":
                section_count += 1
                # 第一个板块标题不需要上方间距（紧贴头部）
                body.append(f'<div class="section-title{" first" if section_count == 1 else ""}">{inline(val)}</div>')
            elif tag in ("@project", "@job", "@edu"):
                if "|" not in val:
                    die(f'{tag} 行需要 " | " 分隔日期：{val}', i)
                left, _, date = val.rpartition("|")
                body.append(
                    f'<div class="item-head"><span class="idate">{inline(date.strip())}</span>'
                    f'<span class="iname">{inline(left.strip())}</span></div>'
                )
            elif tag == "@desc":
                body.append(f'<div class="item-desc">{inline(val)}</div>')
            elif tag == "@stack":
                body.append(f'<div class="item-stack"><b>技术架构：</b>{inline(val)}</div>')
            elif tag == "@text":
                body.append(f'<div class="item-desc">{inline(val)}</div>')
            else:
                die(f"不认识的标记 {tag}（可用：@name/@title/@contact/@section/@project/@job/@edu/@desc/@stack/@text/@样式）", i)
        elif ln.startswith("- "):
            if section_count == 0:
                die("列表条目必须出现在 @section 之后", i)
            cur_list.append(f"<li>{inline(ln[2:])}</li>")
        else:
            die(f"无法解析的行（不是 @ 标记、不是 '- ' 列表、不是空行）：{ln[:40]}", i)

    flush_list()
    if not (header["name"] and header["title"] and header["contact"]):
        die("缺少 @name/@title/@contact 头部信息")

    header_html = (
        '<div class="header">'
        f'<div class="name">{header["name"]}</div>'
        f'<div class="title-line">{header["title"]}</div>'
        f'<div class="contact">{header["contact"]}</div>'
        "</div>"
    )
    return TEMPLATE.format(s=style, HEADER=header_html, BODY="\n".join(body))


def print_pdf(html_path: pathlib.Path, pdf_path: pathlib.Path):
    browser = next((p for p in BROWSER_CANDIDATES if pathlib.Path(p).exists()), None)
    if browser is None:
        die("找不到 Edge/Chrome，无法打印 PDF（可装 Edge，或手动用浏览器打印 _resume.html）")
    r = subprocess.run(
        [browser, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
         f"--print-to-pdf={pdf_path}", f"file://{html_path}"],
        capture_output=True, timeout=60,
    )
    if not pdf_path.exists():
        die(f"浏览器打印失败: {r.stderr.decode(errors='replace')[:200]}")


def main():
    if not MASTER.exists():
        die(f"找不到正本文件 {MASTER}")
    html_text = parse_and_render(MASTER.read_text(encoding="utf-8"))
    OUT_HTML.write_text(html_text, encoding="utf-8")
    print(f"[1/3] HTML 已生成: {OUT_HTML.name}")
    print_pdf(OUT_HTML, OUT_PDF)
    print(f"[2/3] PDF 已生成: {OUT_PDF}")
    print("[3/3] ATS 文本层自检：")
    r = subprocess.run([sys.executable, str(ROOT / "scripts/check_ats.py"), str(OUT_PDF),
                        "C++", "网络编程", "工程师", "MySQL", "Redis", "RabbitMQ", "epoll"])
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
