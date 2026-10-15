// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// ВАЖНО: VMA требует exceptions, поэтому компилируем этот файл с /EHsc
// Этот файл полностью изолирован и НЕ использует stdafx.h или xrCore.h

// Отключаем warnings
#pragma warning(push)
#pragma warning(disable: 4100) // unreferenced formal parameter
#pragma warning(disable: 4189) // local variable is initialized but not referenced

// Определяем Windows и Vulkan макросы напрямую
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

// Настройки VMA для минимальных зависимостей
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION 1003000 // Vulkan 1.3

// VMA leak diagnostics (permanent regression guard). On a clean shutdown these
// emit NOTHING; if any VkBuffer/VkImage outlives vmaDestroyAllocator() they are
// each enumerated into the xray log by name, and the leak is logged rather than
// popping a blocking Debug assert dialog (leak-on-exit is harmless — the OS
// reclaims it — but we want it visible, not fatal). `Msg` lives in xrCore
// (statically linked into the exe); forward-declare it so this otherwise-isolated
// TU can call it without pulling xrCore headers.
#ifdef _MSC_VER
void __cdecl Msg(const char* format, ...);
#else
void Msg(const char* format, ...);
#endif
#define VMA_LEAK_LOG_FORMAT(format, ...)  do { Msg("![VK-VMA-LEAK] " format, __VA_ARGS__); } while (0)
#define VMA_ASSERT_LEAK(expr)             do { if (!(expr)) Msg("![VK-VMA] leak-assert: %s", #expr); } while (0)

// Switch: VMA's block-vector locks as a plain mutex, not std::shared_mutex. 📏11-10: in a fight the render thread stood
// 180-590 ms (rt_wait) next to an 8 MB VMA CreateBlock on a background loader -- vkAllocateMemory in NVK costs ~0.4 ms
// per MB, and 42-53 ms on a 0x3B thread preempted on core 2, all of it with the memory type's lock held for writing.
// libstdc++'s shared_mutex there is a pthread rwlock (mutex + condvar): no priority inheritance, so the 0x2D render
// thread (and the main thread, the second thread) waited for a 0x3B thread the scheduler did not run. std::mutex is
// libnx's Mutex = svcArbitrateLock, where the kernel lends the waiter's priority to the owner.
#ifdef __SWITCH__
#include <mutex>
extern "C" unsigned svcGetThreadPriority(int* priority, unsigned handle); // libnx
// [vma lock]: a wait for one of these locks of 3 ms or more, printed with the waiter's priority
class XrVmaRWMutex
{
public:
    void LockRead() { Lock(); }
    void UnlockRead() { m_Mutex.unlock(); }
    bool TryLockRead() { return m_Mutex.try_lock(); }
    void LockWrite() { Lock(); }
    void UnlockWrite() { m_Mutex.unlock(); }
    bool TryLockWrite() { return m_Mutex.try_lock(); }

private:
    static unsigned long long Ticks()
    {
        unsigned long long t;
        asm volatile("mrs %0, cntpct_el0" : "=r"(t)); // 19.2 MHz
        return t;
    }
    void Lock()
    {
        if (m_Mutex.try_lock())
            return;
        const unsigned long long t0 = Ticks();
        m_Mutex.lock();
        const double ms = double(Ticks() - t0) / 19200.0;
        static int s_printed = 0;
        if (ms >= 3.0 && s_printed < 100)
        {
            ++s_printed;
            int prio = -1;
            svcGetThreadPriority(&prio, 0xFFFF8000u);
            Msg("~ [vma lock] waited %.1f ms (waiter prio 0x%X) #%d", ms, prio, s_printed);
        }
    }
    std::mutex m_Mutex;
};
#define VMA_RW_MUTEX XrVmaRWMutex
#define VMA_USE_STL_SHARED_MUTEX 0
#endif

// Определяем VMA implementation только здесь
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#pragma warning(pop)

// Этот файл служит только для компиляции VMA implementation
// Все функции VMA будут доступны через vk_mem_alloc.h в других файлах
