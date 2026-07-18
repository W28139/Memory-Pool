#pragma once

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>

namespace memoryPool
{
#define MEMORY_POOL_NUM 64      // 内存池的总数量
#define SLOT_BASE_SIZE 8        // plot块的基础大小
#define MAX_SLOT_SIZE 512       // plot块的最大

struct Slot
{
    Slot* next;
};

class MemoryPool
{
public:
    MemoryPool(size_t BlockSize = 4096);
    ~MemoryPool();

    void init(size_t);

    void* allocate();
    void deallocate(void*);

private:
    void allocateNewBlock();
    size_t padPointer(char* p, size_t align);

    void pushFreeList(Slot* slot);
    Slot* popFreeList();
private:
    int        BlockSize_;         // 内存块大小
    int        SlotSize_;          // 槽大小
    Slot*      firstBlock_;        // 指向内存池管理的首个实际内存块
    Slot*      curSlot_;           // 指向当前未被使用过的槽
    Slot*      freeList_;          // 指向空闲的槽(被使用过后又被释放的槽)
    Slot*      lastSlot_;          // 作为当前内存块中最后能够存放元素的位置标识(超过该位置需申请新的内存块)
    std::mutex mutexForBlock_;     // 保护新内存块分配和curSlot_
    std::mutex mutexForFreeList_;  // 保护freeList_的读写，避免ABA问题
};

class HashBucket
{
public:
    static void initMemoryPool();
    static MemoryPool& getMemoryPool(int index);

    static void* useMemory(size_t size)
    {
        if (size <= 0)
            return nullptr;
        if (size > MAX_SLOT_SIZE) // 大于512字节的内存，则使用new
            return operator new(size);

        return getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).allocate();
    }

    static void freeMemory(void* ptr, size_t size)
    {
        if (!ptr)
            return;
        if (size > MAX_SLOT_SIZE)
        {
            operator delete(ptr);
            return;
        }
        getMemoryPool(((size + 7) / SLOT_BASE_SIZE) - 1).deallocate(ptr);
    }

    template<typename T, typename... Args>
    friend T* newElement(Args&&... args);

    template<typename T>
    friend void deleteElement(T* p);
};

template<typename T, typename... Args>
T* newElement(Args&&... args)
{
    T* p = reinterpret_cast<T*>(HashBucket::useMemory(sizeof(T)));
    if (p != nullptr)
        new(p) T(std::forward<Args>(args)...);
    return p;
}

template<typename T>
void deleteElement(T* p)
{
    if (p)
    {
        p->~T();
        HashBucket::freeMemory(reinterpret_cast<void*>(p), sizeof(T));
    }
}

} // namespace memoryPool
