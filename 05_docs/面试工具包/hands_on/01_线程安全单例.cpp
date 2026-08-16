// ============================================================================
// 01_线程安全单例.cpp — 手撕参考：双检锁 DCLP / call_once / Meyers 三版对比
// 2026-08-17 自动生成
//
// 【模块职责】
// 给出三种线程安全单例的完整实现，并用多线程自测验证"只构造一次、地址唯一"：
//   A. DclpSingleton  —— 双检锁 + std::atomic + acquire/release（C++11 内存序正确版）
//   B. CallOnceSingleton —— std::call_once + once_flag
//   C. MeyersSingleton  —— 函数局部静态（最短、生产首选）
//
// 【关键设计决策：C++11 之后为什么不推荐手写 DCLP】
// 1. C++11 之前 DCLP 没有可移植的正确写法：`instance = new Singleton` 在无内存模型
//    的世界里可被拆成"分配→构造→发布指针"三步并被编译器/CPU 重排成"分配→发布→构造"，
//    另一个线程在第一次检查时读到非空指针，拿到的是**半构造对象**。教科书修法 volatile
//    是错的：volatile 只禁编译器缓存/重排该变量的访问，不产生任何 happens-before 同步
//    （对应题库 §13.25"volatile 是不是线程同步手段"）。
// 2. C++11 给了内存模型后，DCLP 用 atomic 的 acquire/release 才真正正确（本文件 A 版），
//    但它仍是"最容易写错的那版"——内存序标错一处就是数据竞争。所以 C++11 之后首选：
//    · 局部静态（C 版）：标准 §6.7 明确规定"并发进入初始化的线程会等待初始化完成"，
//      编译器用 __cxa_guard 之类的锁实现，一行搞定，GCC/Clang 早已支持；
//    · call_once（B 版）：可移植的"只执行一次"原语，比手搓 bool 标志稳在两点——
//      天然内存同步（所有参与线程与初始化完成建立 happens-before），且初始化函数
//      抛异常时 once_flag 保持"未执行"，后续调用会重试而不是永久卡死。
// 3. 面试讲法：先写 C 版（30 秒），再主动说"如果要讲内存序，DCLP 正确版是这样"，
//    把 acquire/release 的配对讲清楚——这是这道题从"背模板"变"懂原理"的分水岭。
//
// 【项目连接】（讲完实现往项目引一句）
// RIS 的索引版本切换（IndexHolder）是单例思想的变体：全局只有一个 shared_ptr 槽位
// 当"唯一访问点"，构建线程 atomic_store 换新版本，检索线程 atomic_load 拷快照——
// 和 DCLP 一样是"全局状态 + 原子操作换内容"，只是换的不是指针创建而是版本切换。
//
// 【已知限制】
// 三个版本都故意不 delete 单例（进程退出交给 OS 回收）：静态对象析构顺序不确定，
// 单例若被其他静态对象在析构期使用反而是坑；Valgrind 里表现为 still reachable，
// 属正常持有（对应题库 §13.27 的口径）。
//
// 编译：g++ -std=c++11 -pthread -Wall -Wextra 01_线程安全单例.cpp -o 01 && ./01
// ============================================================================

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

// ============================================================================
// 版本 A：双检锁（Double-Checked Locking Pattern）+ std::atomic
// ============================================================================
class DclpSingleton {
public:
    // 热路径：构造完成后每次访问只是一次 acquire 原子读（几纳秒），完全不加锁——
    // 这就是"双检"的第一检存在的意义：把昂贵的加锁压缩到首次构造时的一次。
    static DclpSingleton *instance() {
        DclpSingleton *p = inst_.load(std::memory_order_acquire); // 读侧 acquire
        if (p == NULL) {                                         // 第一检（无锁）
            std::lock_guard<std::mutex> lk(ctor_mtx_);
            p = inst_.load(std::memory_order_relaxed);           // 第二检（锁内）
            if (p == NULL) {
                p = new DclpSingleton();
                // store release 与外面的 load acquire 配对：保证"构造函数的所有写入"
                // happens-before 于任何通过指针看到它的读取——这就是堵住"半构造对象"
                // 窗口的那一对内存序。去掉这对（都用 relaxed）程序照样"看起来能跑"，
                // 但那是未定义行为，TSAN 一开就现形。
                inst_.store(p, std::memory_order_release);
            }
        }
        return p;
    }

    // 第二检为什么可以 relaxed：此时已持有 ctor_mtx_，与完成 store 的线程经 mutex
    // 建立 happens-before，不需要原子操作再提供同步，只取值即可——顺带演示"不是所有
    // 原子访问都要最强序"的思考过程。

    DclpSingleton(const DclpSingleton &) = delete;             // 单例禁拷贝
    DclpSingleton &operator=(const DclpSingleton &) = delete;  // C++11 =delete

    int value() const { return val_; }
    static int ctorCount() { return ctor_count_.load(); }

private:
    DclpSingleton() : val_(42) { ctor_count_.fetch_add(1); } // 故意无副作用，方便计数断言

    const int val_;
    static std::mutex ctor_mtx_;              // 只保护"首次构造"这一小段
    static std::atomic<DclpSingleton *> inst_; // 全局唯一槽位
    static std::atomic<int> ctor_count_;
};

std::mutex DclpSingleton::ctor_mtx_;
std::atomic<DclpSingleton *> DclpSingleton::inst_(NULL); // 显式置空：atomic 默认构造不初始化
std::atomic<int> DclpSingleton::ctor_count_(0);

// ============================================================================
// 版本 B：std::call_once + once_flag
// ============================================================================
class CallOnceSingleton {
public:
    // call_once 保证：即使多个线程同时进入，初始化函数只被执行一次，且所有线程
    // 都会等到它执行完才返回（返回即完成）——调用方拿到 inst_ 时一定构造完毕，
    // 这个同步是标准保证的，不需要自己动内存序。
    static CallOnceSingleton *instance() {
        std::call_once(flag_, []() { inst_ = new CallOnceSingleton(); });
        return inst_;
    }

    CallOnceSingleton(const CallOnceSingleton &) = delete;
    CallOnceSingleton &operator=(const CallOnceSingleton &) = delete;

    static int ctorCount() { return ctor_count_.load(); }

private:
    CallOnceSingleton() { ctor_count_.fetch_add(1); }

    static std::once_flag flag_;  // once_flag 本身就是同步点（底层通常是 pthread_once）
    static CallOnceSingleton *inst_;
    static std::atomic<int> ctor_count_;
};

std::once_flag CallOnceSingleton::flag_;
CallOnceSingleton *CallOnceSingleton::inst_ = NULL;
std::atomic<int> CallOnceSingleton::ctor_count_(0);

// ============================================================================
// 版本 C：Meyers 单例（函数局部静态）——生产首选
// ============================================================================
class MeyersSingleton {
public:
    // C++11 标准 §6.7：并发进入未完成初始化的局部静态声明时，后到者会阻塞等待，
    // 直到初始化完成。同步由编译器插入的守卫代码保证（GCC/Clang 的 __cxa_guard），
    // 手写代码量最少、最不容易写错。注意：C 版返回引用而不是指针，语义更干净。
    static MeyersSingleton &instance() {
        static MeyersSingleton s; // 全部魔法在这一行
        return s;
    }

    MeyersSingleton(const MeyersSingleton &) = delete;
    MeyersSingleton &operator=(const MeyersSingleton &) = delete;

    static int ctorCount() { return ctor_count_.load(); }

private:
    MeyersSingleton() { ctor_count_.fetch_add(1); }

    static std::atomic<int> ctor_count_;
};

std::atomic<int> MeyersSingleton::ctor_count_(0);

// ============================================================================
// 自测：N 个线程并发锤 instance()，验证 (1) 只构造一次 (2) 所有线程拿到同一地址
// ============================================================================

// 通用的并发锤击函数：get 是"取单例"的可调用对象，返回 void* 便于模板统一处理
template <typename GetFn>
static int hammerAndGetMismatch(GetFn get, int nthreads, int per_thread) {
    std::atomic<void *> first(NULL); // 首个非空地址作为基准
    std::atomic<int> mismatches(0);
    std::vector<std::thread> ts;
    for (int i = 0; i < nthreads; ++i) {
        ts.push_back(std::thread([&]() {
            for (int k = 0; k < per_thread; ++k) {
                void *p = get();
                void *expect = first.load();
                if (expect == NULL) {
                    // CAS 抢基准：失败说明别人先设了，expect 被更新为对方值
                    if (!first.compare_exchange_strong(expect, p)) {
                        if (expect != p) mismatches.fetch_add(1);
                    }
                } else if (expect != p) {
                    mismatches.fetch_add(1);
                }
            }
        }));
    }
    for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
    return mismatches.load();
}

int main() {
    const int kThreads = 8;
    const int kPerThread = 20000;

    int m1 = hammerAndGetMismatch(
        []() -> void * { return DclpSingleton::instance(); }, kThreads, kPerThread);
    int m2 = hammerAndGetMismatch(
        []() -> void * { return CallOnceSingleton::instance(); }, kThreads, kPerThread);
    int m3 = hammerAndGetMismatch(
        []() -> void * { return &MeyersSingleton::instance(); }, kThreads, kPerThread);

    int fail = 0;
    // 每个版本三条断言：构造次数=1、地址无分叉、数据可读（半构造对象读 val_ 会露馅）
    if (DclpSingleton::ctorCount() != 1 || m1 != 0 ||
        DclpSingleton::instance()->value() != 42) {
        std::printf("[FAIL] DCLP 版: ctor=%d mismatch=%d\n",
                    DclpSingleton::ctorCount(), m1);
        ++fail;
    } else {
        std::printf("[PASS] DCLP 版: ctor=1, %d 线程 x %d 次, 地址唯一\n",
                    kThreads, kPerThread);
    }
    if (CallOnceSingleton::ctorCount() != 1 || m2 != 0) {
        std::printf("[FAIL] call_once 版: ctor=%d mismatch=%d\n",
                    CallOnceSingleton::ctorCount(), m2);
        ++fail;
    } else {
        std::printf("[PASS] call_once 版: ctor=1, 地址唯一\n");
    }
    if (MeyersSingleton::ctorCount() != 1 || m3 != 0) {
        std::printf("[FAIL] Meyers 版: ctor=%d mismatch=%d\n",
                    MeyersSingleton::ctorCount(), m3);
        ++fail;
    } else {
        std::printf("[PASS] Meyers 版: ctor=1, 地址唯一\n");
    }

    std::printf(fail == 0 ? "全部通过\n" : "存在失败项\n");
    return fail == 0 ? 0 : 1;
}
