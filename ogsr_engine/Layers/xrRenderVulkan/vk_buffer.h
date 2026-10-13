// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
#include "vk_core.h"

namespace VK
{

/**
 * Vulkan Buffer Wrapper
 *
 * Универсальный класс для работы с буферами (vertex, index, uniform).
 * Использует VMA (Vulkan Memory Allocator) для эффективного управления памятью.
 *
 * Типы буферов:
 * - Vertex Buffer: геометрия (позиции, нормали, UV)
 * - Index Buffer: индексы треугольников
 * - Uniform Buffer: константы для шейдеров (матрицы, параметры)
 * - Staging Buffer: временный буфер для CPU→GPU transfer
 *
 * Usage:
 *   CVulkanBuffer vb;
 *   vb.Create(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO);
 *   vb.Upload(vertexData, size);
 *   vb.Destroy();
 */
class CVulkanBuffer
{
public:
    CVulkanBuffer();
    ~CVulkanBuffer();

    /**
     * Создать буфер
     * @param size Размер буфера в байтах
     * @param usage Usage flags (VERTEX_BUFFER, INDEX_BUFFER, UNIFORM_BUFFER, etc.)
     * @param memUsage VMA memory usage hint:
     *                 - VMA_MEMORY_USAGE_AUTO (рекомендуется)
     *                 - VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE (GPU-only, быстро)
     *                 - VMA_MEMORY_USAGE_AUTO_PREFER_HOST (CPU-доступ, медленно)
     * @param gpuOnly TRUE = требовать DEVICE_LOCAL и НЕ запрашивать host-доступ.
     *                ВАЖНО: без этого ЛЮБОЙ storage-буфер получает
     *                HOST_ACCESS_SEQUENTIAL_WRITE (см. Create) — VMA тогда обязан
     *                выбрать HOST_VISIBLE память, т.е. 256MB BAR-кучу или, когда она
     *                занята, СИСТЕМНУЮ RAM: шейдер читает такой буфер по PCIe
     *                (диагноз Bins/Tree* 20-30ms на Припяти). Ставь TRUE для буферов,
     *                которые пишет/читает только GPU (CPU-путь — через Upload/Fill).
     *                Map() у такого буфера вернёт nullptr (Upload сам уйдёт в staging).
     */
    // `computeShared`: the buffer is touched by BOTH the graphics and the dedicated
    // compute queue (async compute, r_async). Its contents then survive the family
    // switch only under CONCURRENT sharing — see the same flag on VK::ImageDesc.
    // No-op when the compute family aliases graphics. Cost is negligible for buffers
    // (no framebuffer compression to lose), unlike images.
    // `hostRead`: the CPU READS this mapping (a readback staging target). Without
    // it a host-visible buffer is allocated write-combined, where reads run at
    // ~230 MB/s — a 21 MB tree readback spent 92 ms in its memcpy, against 3 ms
    // of actual GPU wait. HOST_ACCESS_RANDOM asks VMA for CACHED memory instead.
    void Create(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage, bool gpuOnly = false,
                bool computeShared = false, bool hostRead = false);

    /** VMA allocation handle (диагностика размещения: vmaGetAllocationMemoryProperties). */
    VmaAllocation GetAllocation() const { return m_Allocation; }

    /**
     * Уничтожить буфер
     */
    void Destroy();

    /**
     * Загрузить данные в буфер (CPU → GPU)
     * @param data Указатель на данные
     * @param size Размер данных в байтах
     * @param offset Смещение в буфере (default: 0)
     *
     * Для небольших данных (<65KB) использует прямой memcpy.
     * Для больших данных использует staging buffer + vkCmdCopyBuffer.
     */
    void Upload(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);

    /**
     * Map buffer memory (получить CPU-доступный указатель)
     * @return Указатель на mapped память или nullptr при ошибке
     *
     * ВАЖНО: После Map() необходимо вызвать Unmap()!
     * Используйте для persistent-mapped uniform buffers.
     */
    void* Map();

    /**
     * Unmap buffer memory
     */
    void Unmap();

    /**
     * Flush mapped memory (для non-coherent memory)
     * Гарантирует видимость изменений на GPU стороне.
     */
    void Flush();

    /**
     * Invalidate mapped memory (для non-coherent memory)
     * Гарантирует видимость изменений от GPU на CPU стороне.
     */
    void Invalidate();

    /**
     * Получить VkBuffer handle
     */
    VkBuffer GetHandle() const { return m_Buffer; }

    /**
     * Получить размер буфера
     */
    VkDeviceSize GetSize() const { return m_Size; }

    /**
     * Проверить создан ли буфер
     */
    bool IsValid() const { return m_Buffer != VK_NULL_HANDLE; }

    /**
     * Проверить mapped ли буфер
     */
    bool IsMapped() const { return m_Mapped != nullptr; }

    /**
     * Get buffer device address (requires VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
     */
    VkDeviceAddress GetDeviceAddress() const;

    /**
     * Заменить содержимое ЭТОГО объекта буфером other (in-place swap для
     * компакции пулов): наш старый VkBuffer уничтожается, все поля (включая
     * usage/memUsage) переезжают из other, other обнуляется. Держатели
     * указателя CVulkanBuffer* автоматически видят новый буфер; сырые
     * VkBuffer-снапшоты (GetHandle) должны патчиться отдельно.
     * ВНИМАНИЕ: GPU обязан не читать старый буфер (queue idle / load-time).
     */
    void AdoptFrom(CVulkanBuffer& other);

private:
    /**
     * Upload с использованием staging buffer (для больших данных)
     */
    void UploadViaStaging(const void* data, VkDeviceSize size, VkDeviceSize offset);

public:
    VkBuffer      m_Buffer     = VK_NULL_HANDLE;  // Vulkan buffer handle
    VmaAllocation m_Allocation = VK_NULL_HANDLE;  // VMA allocation handle
    VkDeviceSize  m_Size       = 0;                // Buffer size
    void*         m_Mapped     = nullptr;          // Mapped pointer (если mapped)

private:
    VkBufferUsageFlags m_Usage    = 0;     // Usage flags
    VmaMemoryUsage     m_MemUsage = VMA_MEMORY_USAGE_AUTO;  // Memory usage
};

} // namespace VK
