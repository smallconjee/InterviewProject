#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_reports.py — 生成 RIS 检查报告测试数据（宿主机运行，写入 testdata/ris/reports/）

覆盖场景：多患者/多模态/多病种正文（供分词与 TF-IDF 排序）、部分带
study_instance_uid 关联 PACS 张三的检查、部分缺失该字段（验证边界）、
少量坏文件（缺必填/非 XML/重复 report_id）。
"""
import os
import random

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "testdata", "ris", "reports")
random.seed(42)

# 与 PACS 测试 DICOM 一致的 UID（张三的检查 → has_image 应为 true）
ZHANG_STUDY_UID = "1.2.826.0.1.3680043.2.1143.1"

PATIENTS = [
    ("P001", "张三", "M", ZHANG_STUDY_UID),   # 带影像关联
    ("P002", "李四", "F", ""),
    ("P003", "王芳", "F", ""),
    ("P004", "赵强", "M", ""),
    ("P005", "陈静", "F", ""),
]

# 病种语料模板：结论 + 影像所见（保证不同病种的关键词有区分度）
FINDINGS = [
    ("两肺纹理清晰，未见明显实质性病变，气管及主支气管通畅。",
     "胸廓对称，气管居中，双肺纹理走行自然，肺野透亮度良好，纵隔居中，心影大小形态正常。"),
    ("右肺上叶见磨玻璃结节，大小约 6mm，边界清楚，考虑炎性结节可能，建议随访。",
     "右肺上叶前段见磨玻璃密度结节影，边界清楚，其内密度均匀，余双肺未见明显异常密度影。"),
    ("左肺下叶占位性病变，考虑肿瘤性病变可能，建议进一步增强检查。",
     "左肺下叶见软组织肿块影，大小约 32mm x 28mm，呈分叶状，边缘见短毛刺，邻近胸膜牵拉。"),
    ("腰椎退行性变，L4/5 椎间盘膨出，硬膜囊受压。",
     "腰椎生理曲度变直，椎体边缘骨质增生，L4/5 椎间盘向周围膨出，硬膜囊前缘受压，椎管有效矢状径约 11mm。"),
    ("右侧股骨颈骨折，断端移位明显，建议手术治疗。",
     "右侧股骨颈骨质不连续，断端向上移位，颈干角变小，周围软组织肿胀。"),
    ("肝右叶囊肿，大小约 18mm，边界清楚，余肝实质内未见异常密度灶。",
     "肝右叶见类圆形低密度灶，边界清楚，密度均匀，CT 值约 12HU，增强扫描未见强化。"),
    ("双侧基底节区腔隙性脑梗死，脑白质变性，脑萎缩改变。",
     "双侧基底节区见点片状低密度影，边界清楚，脑室系统扩大，脑沟脑裂增宽加深，中线结构居中。"),
    ("慢性支气管炎伴肺气肿改变，双肺散在炎症。",
     "双肺纹理增粗紊乱，双肺野透亮度增高，双肺散在斑片状模糊影，以双下肺为著。"),
    ("右肾结石，大小约 7mm，伴右肾轻度积水。",
     "右肾盂内见结节状高密度影，边缘光滑，右肾盂肾盏轻度扩张积水，左肾形态密度未见异常。"),
    ("甲状腺右侧叶结节，TI-RADS 3 类，建议定期复查。",
     "甲状腺右侧叶见低回声结节，大小约 9mm x 7mm，边界清楚，形态规则，内部回声均匀。"),
]

DOCTORS = ["李建国", "王雪梅", "周明", "刘洋"]


def report_xml(rid, pid, name, sex, study_uid, date, modality, part, doctor, conclusion, desc):
    study_line = f"  <study_instance_uid>{study_uid}</study_instance_uid>\n" if study_uid else ""
    return f"""<report>
  <report_id>{rid}</report_id>
{study_line}  <patient>
    <patient_id>{pid}</patient_id>
    <patient_name>{name}</patient_name>
    <sex>{sex}</sex>
    <birth_date>1960-01-01</birth_date>
  </patient>
  <exam>
    <exam_date>{date}</exam_date>
    <modality>{modality}</modality>
    <exam_part>{part}</exam_part>
    <hospital>示例医院</hospital>
    <doctor>{doctor}</doctor>
  </exam>
  <diagnosis>
    <conclusion>{conclusion}</conclusion>
    <description>{desc}</description>
  </diagnosis>
</report>
"""


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    n = 0
    for pid, name, sex, study_uid in PATIENTS:
        for k in range(10):
            concl, desc = FINDINGS[(k + hash(pid)) % len(FINDINGS)]
            modality = random.choice(["CT", "MR", "DR"])
            part = random.choice(["胸部", "腰椎", "头颅", "腹部", "骨盆"])
            date = f"2026-{random.randint(1, 8):02d}-{random.randint(1, 28):02d}"
            rid = f"RPT-2026-{n + 1:04d}"
            # 张三第一份报告带 study_uid（关联 PACS），其余按 study_uid 是否有值随机
            uid = study_uid if (pid == "P001" and k == 0) else (study_uid if random.random() < 0.2 else "")
            xml = report_xml(rid, pid, name, sex, uid, date, modality, part,
                             random.choice(DOCTORS), concl, desc)
            with open(os.path.join(OUT_DIR, f"{rid}.xml"), "w", encoding="utf-8") as f:
                f.write(xml)
            n += 1
    print(f"生成 {n} 份正常报告")

    # 坏文件 1：缺必填 conclusion
    with open(os.path.join(OUT_DIR, "BAD-001.xml"), "w", encoding="utf-8") as f:
        f.write("<report><report_id>BAD-001</report_id><patient><patient_id>X</patient_id>"
                "</patient><diagnosis></diagnosis></report>")
    # 坏文件 2：非 XML
    with open(os.path.join(OUT_DIR, "BAD-002.xml"), "w", encoding="utf-8") as f:
        f.write("this is not xml at all {{{")
    # 坏文件 3：与 RPT-2026-0001 重复的 report_id（构建时应被去重）
    with open(os.path.join(OUT_DIR, "BAD-003-dup.xml"), "w", encoding="utf-8") as f:
        f.write(report_xml("RPT-2026-0001", "P009", "重复者", "M", "", "2026-01-01",
                           "CT", "胸部", "李建国", FINDINGS[0][0], FINDINGS[0][1]))
    print("生成 3 个坏文件（缺字段/非XML/重复ID）")
    print(f"输出目录: {OUT_DIR}")


if __name__ == "__main__":
    main()
