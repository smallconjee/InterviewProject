// ============================================================================
// 02_线程池.cpp — 手撕参考：固定 worker + 任务队列 + condition_variable + 优雅关闭
// 2026-08-17 自动生成
//
// 【模块职责】
// 最小但完整的固定大小线程池：构造时启动 N 个 worker，共享一条任务队列，
// enqueue 投递任务，析构时优雅关闭（排干队列再收线程）。main() 自测验证
// 任务不丢、不多执行、析构时全部收尾。
//
// 【面试讲什么——写之前先说结构，边写边讲下面五个点】
// 1. 整体结构三件套：任务队列（装 std::function<void()>，类型擦除统一任务形状）、
//    一把 mutex（保护队列和 stop 标志）、一个 condition_variable（队列空时让
//    worker 睡觉，来活了叫醒一个）。
// 2. 虚假唤醒（spurious wakeup）：操作系统底层 fwait 允许 cv 无 notify 也返回
//    （性能/信号伪唤醒等原因），所以 wait 必须配谓词循环：wait(lk, pred) 完全等价于
//    while (!pred()) wait(lk)。如果裸写 if + wait，线程醒来拿到的可能是空队列，
//    front() 直接未定义行为——这是本题最常见的挂法。
// 3. 谓词为什么是 `stop_ || !tasks_.empty()` 而不是只有 `!tasks_.empty()`：
//    析构置 stop_ 后要 notify_all 把所有睡着的 worker 叫醒退出；谓词里没有 stop_
//    的话，worker 醒来发现队列空、条件不满足继续睡——永远退不出，析构的 join 卡死。
//    反过来退出条件写 `stop_` 就退（不排队列）是"丢弃策略"，写 `stop_ && empty()`
//    才退（本实现，排干策略）——备份类任务不能丢，选排干；请求类任务可以丢，选快退。
//    两种策略都能讲，关键是"知道自己选了哪种、为什么"。
// 4. 池大小怎么定：CPU 密集型 ≈ 核数（多了只剩上下文切换）；IO 密集型 ≈ 核数 x
//    (1 + 等待/计算)。拿项目里 RIS 的推导当实例（题库 07 §8.2）：生产 4C8G 虚机、
//    峰值 20 QPS、单查询 P99 80ms → 在飞请求约 20x0.08=1.6 个，单线程串行能力约
//    12 QPS，4 线程理论上限 50 QPS，对峰值留 2.5 倍余量——线程数是推出来的不是拍的。
// 5. 任务队列用什么容器：std::queue 默认 deque 底层——头删尾插双端 O(1)、分段连续
//    对缓存友好，FIFO 天然公平；要任务优先级换 priority_queue；要固定内存用环形
//    缓冲（见 03 生产者消费者的有界队列）。无界队列要警惕：入队速度持续大于消费
//    速度时内存无限涨——生产上要配上界或监控（对应我们 RabbitMQ 备份队列的积压
//    告警：深度 >2 万且持续 1 小时）。
//
// 【实现细节的"为什么"】
// - 取任务 std::move 出来再 unlock 才执行：临界区只覆盖队列操作，任务执行（可能
//   毫秒到秒级）不占锁，其他 worker 才能并发取活。
// - worker 里 task() 包 try/catch：任务抛异常如果不吞，异常逃出线程函数该 worker
//   直接终止，池子慢性漏线程——生产线程池必须兜（muduo 同款处理）。
// - enqueue 返回 bool 而不是抛异常：关闭后拒绝新任务是正常生命周期语义，用返回值
//   让调用方决定怎么处理（比异常轻、比静默丢弃诚实）。
// - 拿不到任务返回值？C++11 的答案是 packaged_task + future：把 task 的返回值
//   塞进 future 交给调用方——面试主动提这一句是加分项，本文件不展开实现。
//
// 【已知限制】
// - 固定 worker 数，无动态扩缩（讲的时候主动认：动态池要处理空闲线程回收和惊群，
//   固定池 + 队列上界在生产上通常够用）；无优先级；无 future 接口。
//
// 编译：g++ -std=c++11 -pthread -Wall -Wextra 02_线程池.cpp -o 02 && ./02
// ============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
    typedef std::function<void()> Task;

    // 构造即启动全部 worker（资源在构造函数里获取、析构里释放——RAII 类）
    explicit ThreadPool(size_t num_workers) : stop_(false) {
        for (size_t i = 0; i < num_workers; ++i) {
            workers_.push_back(std::thread(&ThreadPool::workerLoop, this));
        }
    }

    // 优雅关闭（排干策略）：置 stop、叫醒所有人、join 全部 worker。
    // worker 的退出条件是 `stop_ && 队列空`，所以析构前投进去的任务会被执行完。
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true; // 必须在锁内改：stop_ 与队列由同一把锁保护，避免撕裂判断
        }
        cv_.notify_all(); // 叫醒所有睡着的 worker，让它们重新评估退出条件
        for (size_t i = 0; i < workers_.size(); ++i) {
            workers_[i].join();
        }
    }

    // 投递任务；池已关闭返回 false（任务未入队，调用方自行处理）
    bool enqueue(Task task) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_) return false;
            tasks_.push(std::move(task)); // move：function 对象可能持有大数据，避免拷贝
        }
        cv_.notify_one(); // 锁外 notify：谓词循环保证了正确性，锁外少一次无谓的锁交接
        return true;
    }

    ThreadPool(const ThreadPool &) = delete;            // 持有线程和锁，语义上不可拷贝
    ThreadPool &operator=(const ThreadPool &) = delete;

private:
    void workerLoop() {
        for (;;) {
            std::unique_lock<std::mutex> lk(mtx_);
            // 谓词含 stop_：关闭时也能被唤醒退出（详见文件头讲点 2、3）
            cv_.wait(lk, [this]() { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) return; // 排干策略：队列空了才退
            Task task = std::move(tasks_.front());
            tasks_.pop();
            lk.unlock(); // 先解锁再执行：任务不占锁（详见文件头"为什么"）
            try {
                task();
            } catch (const std::exception &e) {
                // 吞任务异常保 worker 不死；生产上这里应打错误日志
                std::printf("[worker] 任务异常被兜住: %s\n", e.what());
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;        // deque 底层的 FIFO（详见文件头讲点 5）
    std::mutex mtx_;                // 保护 tasks_ 和 stop_
    std::condition_variable cv_;    // 队列从空变非空 / 置 stop 时唤醒 worker
    bool stop_;                     // 关闭标志（锁内读写）
};

// ============================================================================
// 自测：4 worker 跑 200 个带小延时任务，验证不丢不重；再验证任务异常不杀 worker；
// 最后靠析构验证优雅关闭把队列排干。
// ============================================================================
int main() {
    const int kWorkers = 4;
    const int kTasks = 200;
    std::atomic<int> done(0);
    std::atomic<long long> sum(0); // atomic<long long> 的 += 用 fetch_add 实现
    int fail = 0;

    {
        ThreadPool pool(kWorkers);
        // 先投一个必抛异常的任务：后续 200 个任务若仍全部完成，
        // 即证明 worker 吞掉异常后继续干活（没有线程伤亡）
        pool.enqueue([]() { throw std::runtime_error("故意抛出的任务异常"); });
        for (int i = 0; i < kTasks; ++i) {
            pool.enqueue([&done, &sum, i]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 模拟耗时
                done.fetch_add(1);
                sum.fetch_add(i);
            });
        }

        // pool 离开作用域：析构排干队列 + join，能走到这里就说明没有卡死
    }

    const int expect_done = kTasks; // 异常任务不计数（抛出前没做任何累加）
    const long long expect_sum = 1LL * (kTasks - 1) * kTasks / 2; // 0+1+...+199

    if (done.load() != expect_done) {
        std::printf("[FAIL] 任务计数: 期望 %d, 实际 %d（丢任务或 worker 死亡）\n",
                    expect_done, done.load());
        ++fail;
    } else {
        std::printf("[PASS] 首个任务抛异常后 %d 个任务仍全部执行, worker 无伤亡\n",
                    expect_done);
    }
    if (sum.load() != expect_sum) {
        std::printf("[FAIL] 求和校验: 期望 %lld, 实际 %lld\n", expect_sum, sum.load());
        ++fail;
    } else {
        std::printf("[PASS] 求和校验 = %lld\n", expect_sum);
    }

    std::printf(fail == 0 ? "全部通过（析构优雅关闭未卡死）\n" : "存在失败项\n");
    return fail == 0 ? 0 : 1;
}
