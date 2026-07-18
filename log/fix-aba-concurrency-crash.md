# 修复高并发下 ABA 问题导致的崩溃

**日期**: 2026-07-18
**问题**: 内存池在多线程高并发场景下随机崩溃 (segfault)
**根因**: 空闲链表 `freeList_` 的无锁 CAS 实现存在 ABA 问题

---

## 问题分析

### 原始实现：为什么用 Lock-Free CAS 栈

内存池的 `freeList_` 是所有线程归还和获取空闲 slot 的唯一入口。每次 `allocate()` / `deallocate()` 都会访问它，是高竞争热点。设计者选择 **Lock-Free Stack** 而不是 `std::mutex`，基于以下考量：

**1. 避免锁竞争导致的上下文切换**

`std::mutex` 在竞争激烈时，未能获取锁的线程会进入内核态阻塞（futex `FUTEX_WAIT`），触发上下文切换。一次上下文切换的代价约为 1~5μs，频繁发生时成为吞吐瓶颈。Lock-Free 方案的核心思想是：**线程永远在用户态自旋，用 CPU 时间换延迟**。

**2. CAS (Compare-And-Swap) 原语**

x86-64 的 `LOCK CMPXCHG` 指令是硬件级别的原子操作，保证：
- **原子性**：读取-比较-写入三步不可分割，不会被其他核打断
- **可见性**：写入立即对其他核可见（缓存一致性协议 MESI）

`std::atomic<T>::compare_exchange_weak(expected, desired)` 对应这个指令：

```cpp
// C++ 写法
freeList_.compare_exchange_weak(oldHead, slot);

// 等价于 x86-64 伪代码
// atomically {
//     if (*freeList_ == oldHead) {
//         *freeList_ = slot;
//         return true;
//     } else {
//         oldHead = *freeList_;
//         return false;
//     }
// }
```

`compare_exchange_weak`（弱版本）在某些平台上可能**虚假失败**——即使 `*freeList_ == oldHead` 也返回 false。因此必须包在 `while(true)` 循环里重试。性能上 `weak` 比 `strong` 少一条指令，适合这种重试场景。

**3. Memory Order 语义**

原始代码为每个原子操作指定了不同的内存序：

```cpp
// ---- pushFreeList ----
Slot* oldHead = freeList_.load(std::memory_order_relaxed);    // (a) 只需原子读，不要求顺序
slot->next.store(oldHead, std::memory_order_relaxed);         // (b) 下一行 CAS 会保证可见性
if (freeList_.compare_exchange_weak(oldHead, slot,
    std::memory_order_release,   // (c) CAS 成功：本线程所有写对后续 acquire 线程可见
    std::memory_order_relaxed))  // (d) CAS 失败：仅更新 oldHead，不需要同步
    return true;

// ---- popFreeList ----
Slot* oldHead = freeList_.load(std::memory_order_acquire);    // (e) 与 (c) 的 release 配对
Slot* newHead = oldHead->next.load(std::memory_order_relaxed);// (f) 下一行 CAS 保证可见性
if (freeList_.compare_exchange_weak(oldHead, newHead,
    std::memory_order_acquire,   // (g) 与 (c) release 配对，保证读到完整数据
    std::memory_order_relaxed))  // (h) 失败仅更新 oldHead
    return oldHead;
```

形成正确的 **Release-Acquire 配对**：push 线程的 release (c) 同步给 pop 线程的 acquire (e)/(g)，保证 push 线程对 `slot->next` 的写入对 pop 线程必定可见。如果全部用默认的 `seq_cst`，x86 上会多出不必要的 `MFENCE`，而 `relaxed` + `release/acquire` 只在必要处插入内存屏障。

**4. 无锁栈的数据结构**

```
   freeList_           ┌──────────┐     ┌──────────┐     ┌──────────┐
   ┌──────┐      ┌────→│ Slot C   │────→│ Slot B   │────→│ Slot A   │──→ nullptr
   │ head │──────┘     │ next     │     │ next     │     │ next     │
   └──────┘            └──────────┘     └──────────┘     └──────────┘
                      (栈顶：最新归还)                  (栈底：最早归还)
```

- **push**：新 slot 插入头部（LIFO），CPU 缓存友好——近期归还的 slot 大概率还在 L1/L2 缓存里
- **pop**：从头部取出，O(1)

**5. 完整代码**

```cpp
// 无锁入队 — CAS 自旋直到成功
bool MemoryPool::pushFreeList(Slot* slot)
{
    while (true)
    {
        Slot* oldHead = freeList_.load(std::memory_order_relaxed);
        slot->next.store(oldHead, std::memory_order_relaxed);

        if (freeList_.compare_exchange_weak(oldHead, slot,
            std::memory_order_release, std::memory_order_relaxed))
        {
            return true;
        }
    }
}

// 无锁出队 — CAS 自旋直到成功或队列为空
Slot* MemoryPool::popFreeList()
{
    while (true)
    {
        Slot* oldHead = freeList_.load(std::memory_order_acquire);
        if (oldHead == nullptr)
            return nullptr;

        Slot* newHead = nullptr;
        try
        {
            newHead = oldHead->next.load(std::memory_order_relaxed);
        }
        catch(...)
        {
            continue;
        }

        if (freeList_.compare_exchange_weak(oldHead, newHead,
            std::memory_order_acquire, std::memory_order_relaxed))
        {
            return oldHead;
        }
    }
}
```

### Bug 原因：ABA 问题

以上实现看起来正确——CAS 保证了原子性，Release-Acquire 保证了内存可见性。**但 Lock-Free Stack 的正确性有一个隐藏前提：指针值 "A" 意味着 "同一个节点 A"。这个前提在内存池场景下不成立。**

内存池中 slot 被反复分配和归还，内存地址被循环使用。指针值 A 无法区分"从来没有被动过的 A"和"被取走、使用、归还了 N 次的 A"。

#### ABA 攻击时序

```
初始: freeList_ → A → B → C → D

T1 (popFreeList):
  oldHead = A, newHead = B    ← 读到 A->next = B
  ⏸️  被抢占，CAS 还没执行

T2: pop A → freeList_ = B → C → D
T2: pop B → freeList_ = C → D           (T2 持有 A, B)
T2: 使用 B，写入用户数据                  (B->next 位置被覆盖为垃圾)
T2: push A → A->next = C → freeList_ = A → C → D

T1: ▶️  CAS(freeList_, A, B) → freeList_ 的值恰好又是 A → 成功!
    freeList_ = B                        (B 还被 T2 占用，未归还！)
    T1 返回 A
```

#### 后果

```
freeList_ = B（B 被 T2 占用中，B->next 是用户写入的垃圾数据）

T3 popFreeList():
  oldHead = B
  newHead = B->next    ← 读到的是垃圾值 0xDEADBEEF
  CAS(freeList_, B, 0xDEADBEEF) → 成功
  freeList_ = 0xDEADBEEF  ← 野指针！

之后任何 allocate() → popFreeList() → 访问 0xDEADBEEF → 💥 SEGFAULT
同时 T2 + T3 拿到了同一块内存 → 数据互相覆盖 → 逻辑崩溃
```

#### 为什么常规 Lock-Free Stack 不容易触发 ABA

在 `new`/`delete` 场景中，节点用完即释放给 OS，OS 不会立即重用同一地址。而**内存池恰恰是循环复用同一块地址**——slot 被归还后立刻可以被重新分配出去，大大提高了 ABA 窗口期的命中概率。这正是内存池的特性和 Lock-Free 的 ABA 脆弱性相冲突的地方。

---

## 修复方案

### 方案选择：放弃 Lock-Free，改用 Mutex

| 候选方案 | 复杂度 | 性能 | 可维护性 |
|---------|--------|------|---------|
| Tagged Pointer (128-bit CAS) | 高 — 需内联汇编 `CMPXCHG16B` | 最优 | 差 — 平台相关，移植困难 |
| Hazard Pointer / EBR | 高 — 需引用计数或 epoch 回收 | 很好 | 差 — 引入第三方库依赖 |
| 内存池内置版本号 | 中 — 复用指针高位存版本 | 好 | 中 — 48 位地址限制 |
| **Mutex** | **低** | **足够好** | **优** |

选择 Mutex 的理由：
1. 64 个独立内存池，每个有自己的 `mutexForFreeList_`，锁竞争被哈希打散
2. 临界区仅 2 行赋值，持有锁的时间极短（< 10ns）
3. 代码行数从 60 行降到 10 行，可读性大幅提升
4. 830 万 ops/s 的实测吞吐量证明性能完全满足需求

### 具体修改

| 位置 | 修改前 | 修改后 |
|------|--------|--------|
| `Slot::next` | `std::atomic<Slot*>` | `Slot*` |
| `freeList_` | `std::atomic<Slot*>` | `Slot*` |
| `MemoryPool` 成员 | — | 新增 `std::mutex mutexForFreeList_` |
| `pushFreeList()` | 15 行 CAS 自旋 | 4 行 mutex lock |
| `popFreeList()` | 22 行 CAS 自旋 | 6 行 mutex lock |

```cpp
// 修改后 — mutex 保护的简单链表操作
void MemoryPool::pushFreeList(Slot* slot)
{
    std::lock_guard<std::mutex> lock(mutexForFreeList_);
    slot->next = freeList_;
    freeList_ = slot;
}

Slot* MemoryPool::popFreeList()
{
    std::lock_guard<std::mutex> lock(mutexForFreeList_);
    if (freeList_ == nullptr)
        return nullptr;
    Slot* slot = freeList_;
    freeList_ = slot->next;
    return slot;
}
```

`std::lock_guard` 在构造时 `mutex.lock()`，析构时 `mutex.unlock()`，RAII 保证即使异常也不会死锁。持有锁的线程独占访问 `freeList_`，不存在并发读写，自然不存在 ABA。

---

## 验证

```
=== 压力测试 ===
线程数:   16
运行时间:  5 s
总操作数: 41,724,800
吞吐量:    8,343,291 ops/s
异常数:   0
结果:     ✅ 通过
```

---

## 涉及文件

- `include/MemoryPool.hpp` — `Slot` 结构体、新增 `mutexForFreeList_` 成员
- `src/MemoryPool.cpp` — `pushFreeList()`、`popFreeList()` 重写
