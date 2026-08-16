// ============================================================================
// 04_编辑距离.cpp — 手撕参考：Levenshtein DP（滚动数组）+ UTF-8 码点切分版
// 2026-08-17 自动生成
//
// 【模块职责】
// 三个层次，层层递进（也是面试的推进节奏）：
//   1. editDistanceBytes     —— 朴素 O(n*m) DP + 两行滚动数组（空间 O(min) 的讲法）
//   2. editDistanceUtf8      —— 先按 UTF-8 码点切分再 DP（中文纠错的正确姿势）
//   3. editDistanceUtf8AtMost —— 带 max_dist 提前退出的限界版（BK-tree 查询用）
//
// 【DP 讲法（题库 08 §9.2 的口播稿对应到代码）】
// f[i][j] = a 前 i 个单位变成 b 前 j 个单位的最少操作数（增/删/改）：
//   边界：f[0][j] = j（全插入）、f[i][0] = i（全删除）
//   转移三选一：
//     删 a[i-1]   ：f[i-1][j]   + 1
//     插 b[j-1]   ：f[i][j-1]   + 1
//     改/相同     ：f[i-1][j-1] + (a[i-1] == b[j-1] ? 0 : 1)
// 时间 O(n*m)，空间用 prev/cur 两行交替 swap 压到 O(min(n,m))——注意"算完 swap 再
// 取 prev[m]"：最后一轮的结果在交换后的 prev 里，这是最容易写错的一行。
//
// 【真实踩坑：中文编辑距离按字节算（弹药库 §11 踩坑实录）】
// RIS 纠错链上线后中文纠错空结果，排查到根因：UTF-8 一个汉字 3 字节，按字节算
// 距离，一字之差 = 3 次字节操作：
//   - 一字之差（"骨裂"→"骨折"）        ：字节距离 3 > max_dist=2 → 不召回
//     （裂=e8a382、折=e68a98，三个字节全不同；注意有些字对共享首字节，如
//      拆/折 共享 e6，字节距离只有 2——举例要挑全不同的字对才好算）
//   - 相邻换位（"肺节结"→"肺结节"）    ：码点距离 2（两次替换）；字节距离是 6
//     （两个 3 字节块互换位置 ≈ 删 3 字节再插 3 字节）——同样远超阈值
//   数字校准：弹药库原文写"距离 3"，精确对应的是**一字之差**的情形（1 字 = 3 字节）；
//   "肺节结"相邻换位那对，字节距离实为 6、码点距离 2——面试口述建议用"一字之差
//   按字节就是 3，超过阈值 2"这个算不错的版本，结论不变：中文纠错整体失效。
// 修法：先用码点切分（项目里用 utfcpp，本文件手写等价逻辑），DP 在码点维度做，
// 一字之差距离 1、换位距离 2，纠错正常召回。同族的坑还有"摘要按字节截断切坏
// 多字节字符产生非法 UTF-8"（弹药库 §11 两连击案例）——一句话总结：多字节文本的
// 任何字节级操作（求长、截断、求距离）都要先过一遍"码点边界"检查。
// 另一个细节：项目里非法 UTF-8 输入不让纠错路径崩——utfcpp 抛异常被 catch 后
// 返回大距离 64，等效于不参与召回；本文件的手写解码器把非法字节吞成 U+FFFD
// 替换单位，语义相同（宁可不纠错，不能崩查询）。
//
// 【限界版（AtMost）为什么能提前退出】
// 性质：DP 每一行的最小值随行号单调不减（可用归纳法证：cur[j] 的三个来源
// prev[j]+1、prev[j-1]+cost、cur[j-1]+1 都 >= 上一行最小值）。所以算完一行发现
// 行最小值已超过 limit，最终 f[n][m] 必然超过，直接返回 limit+1 当"超界"哨兵。
// BK-tree 查询的 max_dist 通常只有 1~2，这个剪枝把大量无望的词提前踢掉；
// 更快的还有带状 DP（只算对角带内）和 Myers 位并行 O(n*m/64)——面试提一句即可。
// BK-tree 本体见 04_ris_report_search/src/spell/BkTree.cpp（靠编辑距离满足三角
// 不等式剪枝，35 万词项词典免全量遍历）。
//
// 编译：g++ -std=c++11 -pthread -Wall -Wextra 04_编辑距离.cpp -o 04 && ./04
// （纯计算无线程，-pthread 仅为和其他三个文件命令一致）
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ----------------------------------------------------------------------------
// DP 本体：对任意"单位"序列（字节或码点）算 Levenshtein 距离
// 模板参数 Unit 在调用侧分别是 char（字节版）和 uint32_t（码点版）——
// 算法与字符单位解耦，是"先切分、再 DP"这个设计的直接体现。
// ----------------------------------------------------------------------------
template <typename Unit>
static int editDistanceOnUnits(const std::vector<Unit> &a, const std::vector<Unit> &b) {
    const size_t n = a.size(), m = b.size();
    if (n == 0) return static_cast<int>(m); // 空串边界：全插入
    if (m == 0) return static_cast<int>(n); // 空串边界：全删除

    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j); // 第 0 行

    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i); // 第 0 列：删掉 a 前 i 个
        for (size_t j = 1; j <= m; ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            const int del = prev[j] + 1;      // 删 a[i-1]
            const int ins = cur[j - 1] + 1;   // 插 b[j-1]
            const int sub = prev[j - 1] + cost; // 替换或恰好相同
            int best = del < ins ? del : ins;
            if (sub < best) best = sub;
            cur[j] = best;
        }
        prev.swap(cur); // 滚动：cur 晋升为 prev，旧 prev 内容下一轮直接覆盖
    }
    return prev[m]; // 最后一轮结果在 swap 后的 prev——取错行是高频笔误
}

// 便捷包装：直接收 std::string（字节维度），内部转 vector<char>
static int editDistanceBytes(const std::string &a, const std::string &b) {
    return editDistanceOnUnits(std::vector<char>(a.begin(), a.end()),
                               std::vector<char>(b.begin(), b.end()));
}

// ----------------------------------------------------------------------------
// UTF-8 → 码点序列（手写版，项目里用 utfcpp 库做同一件事）
// 规则：首字节 0xxxxxxx=1B / 110xxxxx=2B / 1110xxxx=3B / 11110xxx=4B，
//       后续字节必须是 10xxxxxx（续字节）。
// 容错：首字节非法或续字节不足/非法时，吞成 U+FFFD 替换单位并前进 1 字节——
//       不抛异常不终止（对应项目"纠错路径不能崩"的口径），坏输入自然拿大距离。
// ----------------------------------------------------------------------------
static std::vector<uint32_t> utf8ToCodepoints(const std::string &s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if ((c & 0x80u) == 0x00u) {         // 0xxxxxxx：ASCII
            len = 1; cp = c;
        } else if ((c & 0xE0u) == 0xC0u) {  // 110xxxxx：2 字节序列首
            len = 2; cp = c & 0x1Fu;
        } else if ((c & 0xF0u) == 0xE0u) {  // 1110xxxx：3 字节（中文主场）
            len = 3; cp = c & 0x0Fu;
        } else if ((c & 0xF8u) == 0xF0u) {  // 11110xxx：4 字节（emoji 等）
            len = 4; cp = c & 0x07u;
        } else {                             // 10xxxxxx 当首字节或 11111xxx：非法
            out.push_back(0xFFFDu);
            ++i;
            continue;
        }
        bool ok = (i + len <= s.size()); // 剩余字节够不够这个序列
        for (size_t k = 1; ok && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0u) != 0x80u) { // 续字节必须是 10xxxxxx
                ok = false;
            } else {
                cp = (cp << 6) | (cc & 0x3Fu); // 码点按 6 bit 一段拼起来
            }
        }
        if (!ok) {
            out.push_back(0xFFFDu); // 坏序列吞成替换单位（见上：不崩、不召回）
            ++i;
        } else {
            out.push_back(cp);
            i += len;
        }
    }
    return out;
}

// 码点维度编辑距离：中文纠错的正确版本（"肺节结"→"肺结节" = 2 的那个 2）
static int editDistanceUtf8(const std::string &a, const std::string &b) {
    return editDistanceOnUnits(utf8ToCodepoints(a), utf8ToCodepoints(b));
}

// ----------------------------------------------------------------------------
// 限界版：距离 <= limit 时返回真实距离，否则返回 limit+1（哨兵值表示"超界"）。
// 依据"行最小值单调不减"提前退出（见文件头【限界版】一节的证明思路）。
// BK-tree 的 suggest 查询就是拿它当度量函数、max_dist 通常取 1~2。
// ----------------------------------------------------------------------------
static int editDistanceUtf8AtMost(const std::string &a, const std::string &b, int limit) {
    const std::vector<uint32_t> ua = utf8ToCodepoints(a);
    const std::vector<uint32_t> ub = utf8ToCodepoints(b);
    const size_t n = ua.size(), m = ub.size();
    if (n == 0 || m == 0) {
        const int d = static_cast<int>(n + m); // 一方为空：距离 = 另一方长度
        return d <= limit ? d : limit + 1;
    }

    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        int row_min = cur[0]; // 跟踪本行最小值，超界即弃
        for (size_t j = 1; j <= m; ++j) {
            const int cost = (ua[i - 1] == ub[j - 1]) ? 0 : 1;
            int best = prev[j] + 1;
            if (cur[j - 1] + 1 < best) best = cur[j - 1] + 1;
            if (prev[j - 1] + cost < best) best = prev[j - 1] + cost;
            cur[j] = best;
            if (cur[j] < row_min) row_min = cur[j];
        }
        if (row_min > limit) return limit + 1; // 剪枝：后面只会更大
        prev.swap(cur);
    }
    return prev[m] <= limit ? static_cast<int>(prev[m]) : limit + 1;
}

// ----------------------------------------------------------------------------
// 自测：经典英文用例 + 中文踩坑案例的数值复现
// ----------------------------------------------------------------------------
static int g_fail = 0;

// 简易断言宏：打印 PASS/FAIL，失败计数（比 assert 好：能一次看全所有用例结果）
#define CHECK_EQ(actual, expected, msg)                                        \
    do {                                                                       \
        const long long a_ = static_cast<long long>(actual);                   \
        const long long e_ = static_cast<long long>(expected);                 \
        if (a_ != e_) {                                                        \
            std::printf("[FAIL] %s: 期望 %lld, 实际 %lld\n", msg, e_, a_);     \
            ++g_fail;                                                          \
        } else {                                                               \
            std::printf("[PASS] %s = %lld\n", msg, a_);                        \
        }                                                                      \
    } while (0)

int main() {
    // --- 英文经典用例（教材答案，先立可信度）---
    CHECK_EQ(editDistanceBytes("kitten", "sitting"), 3, "kitten->sitting (字节)");
    CHECK_EQ(editDistanceBytes("sunday", "saturday"), 3, "sunday->saturday (字节)");
    CHECK_EQ(editDistanceUtf8("kitten", "sitting"), 3, "kitten->sitting (码点)");
    CHECK_EQ(editDistanceBytes("", "abc"), 3, "空串->abc");
    CHECK_EQ(editDistanceBytes("", ""), 0, "空串->空串");

    // --- 中文踩坑案例数值复现（弹药库 §11）---
    // 一字之差（形近打错）：字节 3、码点 1——"按字节算距离 3 超过 max_dist=2"
    // 正是线上纠错空结果的直接原因
    CHECK_EQ(editDistanceBytes("骨裂", "骨折"), 3, "一字之差(字节)=3 超阈值");
    CHECK_EQ(editDistanceUtf8("骨裂", "骨折"), 1, "一字之差(码点)=1 正常召回");

    // 相邻换位（弹药库原案例"肺节结"->"肺结节"）：码点 2、字节 6（两 3 字节块互换）
    CHECK_EQ(editDistanceUtf8("肺节结", "肺结节"), 2, "换位(码点)=2");
    CHECK_EQ(editDistanceBytes("肺节结", "肺结节"), 6, "换位(字节)=6");

    // --- 限界版：BK-tree 查询视角（max_dist=2）---
    CHECK_EQ(editDistanceUtf8AtMost("肺节结", "肺结节", 2), 2, "限界版: 码点 2<=2 召回");
    CHECK_EQ(editDistanceUtf8AtMost("肺节结", "骨折", 2), 3, "限界版: 超界返回 limit+1=3");
    CHECK_EQ(editDistanceUtf8AtMost("肺结节", "肺结节", 0), 0, "限界版: 完全相等");

    std::printf(g_fail == 0 ? "全部通过\n" : "存在失败项\n");
    return g_fail == 0 ? 0 : 1;
}
