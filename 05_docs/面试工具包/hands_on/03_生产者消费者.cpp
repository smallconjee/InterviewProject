// ============================================================================
// 03_生产者消费者.cpp — 手撕参考：有界阻塞队列（mutex + 两个 condition_variable）
// 2026-08-17 自动生成
//
// 【模块职责】
// 实现固定容量的线程安全阻塞队列（BoundedBlockingQueue<T>）：满则 put 阻塞、
// 空则 take 阻塞、close 之后消费者排干剩余数据并退出。main() 用多生产者多消费者
// 自测：总数守恒、求和守恒、优雅收尾不死锁。
//
// 【面试讲什么——边写边讲四个点】
// 1. 为什么是"一把锁 + 两个条件变量"（not_empty / not_full）：
//    一把锁保护队列本身（互斥），两个 cv 是两个等待集合——生产者等"不满"、消费者等
//    "不空"，notify 时各叫各的，不会把同侧线程互相叫醒空转。只写一个 cv 也对（正确性
//    靠谓词循环兜底），但每次 notify_all 都会把两类线程全叫醒重判谓词，白白多几次
//    锁竞争——两个 cv 是"正确性等价、精确度更高"的写法。
// 2. 谓词为什么带 closed_：和线程池的 stop_ 同一个道理——close() 要能把阻塞中的
//    put/take 全叫醒，否则消费者永远睡在"等数据"上，进程收不了尾。put 的谓词是
//    `closed_ || 未满`（满了就睡），take 的谓词是 `closed_ || 非空`（空了就睡）；
//    醒来先判 closed_ 决定退出语义：put 返回 false（拒绝再生产），take 在排干
//    剩余数据后返回 false（数据_eof_ 语义，和管道读端读到 EOF 一个味道）。
// 3. 有界 = 反向压流（backpressure）：容量满了 put 自然阻塞上游，下游处理不过来时
//    压力沿调用链回传而不是内存无限涨。项目连接：备份链的 RabbitMQ 就是跨进程版的
//    生产者消费者——队列有深度上限语义，高峰积压 1~2 万任务、通常 1 小时内回落；
//    监控口径是"积压 >2 万且持续 1 小时"双条件告警（区分正常潮汐和异常积压）。
// 4. notify 放锁内还是锁外：都正确（谓词循环是正确性的根），锁内 notify 简单直白，
//    锁外 notify 省一次"被唤醒线程立刻撞上还没放掉的锁"的空转（hurry-up-and-wait）。
//    本实现 close 里锁内 notify_all（收尾路径不差这一次），put/take 锁外 notify_one
//    （热路径省一点是一点）——能讲出"两种都对、我按路径选"就够了。
//
// 【对比信号量方案】（面试官问"为什么不用信号量"时的话术）
// 经典信号量版是三件套：sem_t empty_slots(容量)、sem_t filled(0)、一把 mutex——
// 生产者 P(empty) → lock/入队/unlock → V(filled)，消费者对称。它的问题：
//   a. 多个谓词条件没法自然表达：本实现的"满/空/已关闭"三个条件在信号量里只有
//      "计数"一个维度，close 想叫醒所有阻塞方且让后续调用直接失败，信号量版要
//      加标志位+额外唤醒逻辑，极易写出唤醒遗漏；
//   b. sem_t 是 POSIX C 接口，无 RAII：sem_wait 会被信号打断（EINTR 要手动重试），
//      忘 sem_destroy 就是泄漏，异常路径更难收拾；macOS 上匿名信号量 sem_init
//      已废弃，可移植性也差；
//   c. 条件变量把"等待的原因"写成谓词，谓词可以是任意布尔表达式（未来加"高水位
//      线降低生产速率"之类的策略不用改同步骨架）——这是表达力上的本质优势。
// 一句话收尾：信号量表达"计数"，条件变量表达"条件"；生产者消费者的等待条件一旦
// 超过"非空/非满"（比如要关闭），条件变量方案扩展成本最低。
//
// 【已知限制】
// - 无优先级、无超时版 put/take（wait_for 加一行超时判断即可，面试提一句就行）；
// - close 后 put 直接 false，不做"排空期间允许继续投递"的精细语义。
//
// 编译：g++ -std=c++11 -pthread -Wall -Wextra 03_生产者消费者.cpp -o 03 && ./03
// ============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

template <typename T>
class BoundedBlockingQueue {
public:
    explicit BoundedBlockingQueue(size_t capacity) : capacity_(capacity), closed_(false) {}

    // 生产者接口：满则阻塞等到有空位；队列已关闭返回 false（该条数据未入队）
    bool put(const T &x) {
        std::unique_lock<std::mutex> lk(mtx_);
        not_full_.wait(lk, [this]() { return closed_ || queue_.size() < capacity_; });
        if (closed_) return false; // 被唤醒后发现已关闭：拒绝生产
        queue_.push(x);
        lk.unlock();
        not_empty_.notify_one(); // 只叫一个消费者：刚好有一个新元素
        return true;
    }

    // 消费者接口：空则阻塞等到有数据；返回 false 表示队列已关闭且已排干（EOF）
    bool take(T &out) {
        std::unique_lock<std::mutex> lk(mtx_);
        not_empty_.wait(lk, [this]() { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false; // 谓词满足但队列空 = closed_ 且排干完毕
        out = queue_.front();
        queue_.pop();
        lk.unlock();
        not_full_.notify_one(); // 腾出一个空位，叫一个阻塞中的生产者
        return true;
    }

    // 关闭队列：唤醒所有阻塞方；已入队的数据仍可被 take 走（排干语义）
    void close() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            closed_ = true;
        }
        not_empty_.notify_all(); // 叫醒等数据的消费者去排干
        not_full_.notify_all();  // 叫醒等空位的生产者去收 false
    }

    BoundedBlockingQueue(const BoundedBlockingQueue &) = delete;
    BoundedBlockingQueue &operator=(const BoundedBlockingQueue &) = delete;

private:
    const size_t capacity_;
    std::queue<T> queue_;         // deque 底层，FIFO 保证生产顺序
    mutable std::mutex mtx_;      // 保护 queue_ 与 closed_
    std::condition_variable not_empty_; // 消费者在此等"非空或关闭"
    std::condition_variable not_full_;  // 生产者在此等"未满或关闭"
    bool closed_;
};

// ============================================================================
// 自测：3 个生产者 x 300 条、2 个消费者、容量 8 的队列。
// 校验三条守恒：总条数 = 900、总和 = 3 x (1..300 之和)（不丢不重不坏）、
// close 之后消费者能正常退出（不死锁）。
// ============================================================================
int main() {
    const int kProducers = 3;
    const int kPerProducer = 300;
    const int kConsumers = 2;
    const size_t kCapacity = 8; // 故意很小：强迫 put/take 反复阻塞唤醒，压谓词逻辑

    BoundedBlockingQueue<int> q(kCapacity);
    std::atomic<int> taken(0);
    std::atomic<long long> sum(0);
    int fail = 0;

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.push_back(std::thread([&q]() {
            for (int i = 1; i <= kPerProducer; ++i) {
                // 忽略 put 返回值：测试里 close 前所有数据都应成功入队
                q.put(i);
                // 小睡模拟生产节奏不均，让"满阻塞"真实发生
                if (i % 17 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }));
    }

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.push_back(std::thread([&q, &taken, &sum]() {
            int v;
            // take 返回 false = 队列关闭且排干：消费者唯一的退出路径
            while (q.take(v)) {
                taken.fetch_add(1);
                sum.fetch_add(v);
            }
        }));
    }

    for (size_t i = 0; i < producers.size(); ++i) producers[i].join();
    q.close(); // 生产者全部完工后关闭；消费者把剩余数据排干后自然退出
    for (size_t i = 0; i < consumers.size(); ++i) consumers[i].join();

    const int expect_count = kProducers * kPerProducer;
    const long long expect_sum =
        1LL * kProducers * (1 + kPerProducer) * kPerProducer / 2; // 3 x (1+...+300)

    if (taken.load() != expect_count) {
        std::printf("[FAIL] 总条数: 期望 %d, 实际 %d（丢数据或重复消费）\n",
                    expect_count, taken.load());
        ++fail;
    } else {
        std::printf("[PASS] %d 生产者 x %d 条经容量 %d 队列, 消费 %d 条, 不丢不重\n",
                    kProducers, kPerProducer, (int)kCapacity, taken.load());
    }
    if (sum.load() != expect_sum) {
        std::printf("[FAIL] 总和: 期望 %lld, 实际 %lld\n", expect_sum, sum.load());
        ++fail;
    } else {
        std::printf("[PASS] 求和守恒 = %lld\n", expect_sum);
    }
    if (consumers[0].joinable() == false && consumers[1].joinable() == false) {
        std::printf("[PASS] close 后消费者排干退出, 无死锁\n");
    }

    std::printf(fail == 0 ? "全部通过\n" : "存在失败项\n");
    return fail == 0 ? 0 : 1;
}
