#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>

#include "include/MemoryPool.hpp"

using namespace memoryPool;
class P1 { int id_; };
class P2 { int id_[5]; };
class P3 { int id_[10]; };
class P4 { int id_[20]; };

// 使用 std::chrono 测量真实耗时（毫秒），避免 clock() 的歧义
template<typename F>
double bench(const char* label, F&& fn, size_t ntimes, size_t nworks, size_t rounds)
{
    std::atomic<size_t> total_ops{0};
    std::vector<std::thread> threads;

    auto t0 = std::chrono::steady_clock::now();

    for (size_t k = 0; k < nworks; ++k) {
        threads.emplace_back([&, ntimes, rounds]() {
            for (size_t j = 0; j < rounds; ++j) {
                fn(ntimes);
                total_ops += ntimes * 4;  // 4 种对象类型
            }
        });
    }
    for (auto& t : threads) t.join();

    auto t1 = std::chrono::steady_clock::now();
    auto wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    auto ops = total_ops.load();
    auto throughput = static_cast<size_t>(ops / (wall_ms / 1000.0));

    printf("%-12s %2lu线程 %2lu轮次 × %4lu次  耗时: %8.2f ms  吞吐: %10lu ops/s\n",
           label, nworks, rounds, ntimes, wall_ms, throughput);
    return wall_ms;
}

void pool_work(size_t n) {
    for (size_t i = 0; i < n; i++) {
        auto* p1 = newElement<P1>(); deleteElement(p1);
        auto* p2 = newElement<P2>(); deleteElement(p2);
        auto* p3 = newElement<P3>(); deleteElement(p3);
        auto* p4 = newElement<P4>(); deleteElement(p4);
    }
}

void raw_work(size_t n) {
    for (size_t i = 0; i < n; i++) {
        auto* p1 = new P1; delete p1;
        auto* p2 = new P2; delete p2;
        auto* p3 = new P3; delete p3;
        auto* p4 = new P4; delete p4;
    }
}

int main()
{
    HashBucket::initMemoryPool();
    // 预热：初始化内存池的 block 和 freeList
    for (int w = 0; w < 100; w++) {
        pool_work(100);
    }

    printf("\n======== 单线程基准 ========\n");
    bench("内存池", pool_work, 1000, 1, 10);
    bench("new/delete", raw_work, 1000, 1, 10);

    printf("\n======== 4线程并发 ========\n");
    bench("内存池", pool_work, 1000, 4, 10);
    bench("new/delete", raw_work, 1000, 4, 10);

    printf("\n======== 10线程并发 ========\n");
    bench("内存池", pool_work, 1000, 10, 10);
    bench("new/delete", raw_work, 1000, 10, 10);

    printf("\n======== 16线程并发 ========\n");
    bench("内存池", pool_work, 1000, 16, 10);
    bench("new/delete", raw_work, 1000, 16, 10);
    return 0;
}