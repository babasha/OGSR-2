#include "stdafx.h"
#include "stream_reader.h"
#include "LocatorAPI.h"     // FS_StreamStats / FS_StreamWindowSize live with the other FS accounting
#include <atomic>
#include <cstdlib>          // getenv — XROS_FS_WINDOW_MB



void CMapStreamReader::construct(const HANDLE& file_mapping_handle, const size_t& start_offset, const size_t& file_size, const size_t& archive_size, const size_t& window_size)
{
    m_file_mapping_handle = file_mapping_handle;
    m_start_offset = start_offset;
    m_file_size = file_size;
    m_archive_size = archive_size;
    m_window_size = _max(window_size, static_cast<size_t>(FS.dwAllocGranularity));

    map(0);
}

void CMapStreamReader::destroy() { unmap(); }

// ---------------------------------------------------------------------------
// Sliding-window accounting. level.geom is 1.96 GB read through a 1 MB window,
// i.e. ~1900 MapViewOfFile/UnmapViewOfFile pairs and ~478k page faults, all on
// the loading thread — measured 1869 MB at 755 MB/s. Two very different causes
// (window churn vs. faulting the pages in) hide behind that one number, so count
// them apart before changing the window size or prefaulting anything.
// ---------------------------------------------------------------------------
namespace {
std::atomic<u64> g_srMaps{0}, g_srMapUs{0}, g_srCopyBytes{0}, g_srCopyUs{0};
}

FS_StreamStats FS_GetStreamStats()
{
    FS_StreamStats s;
    s.maps       = g_srMaps.load(std::memory_order_relaxed);
    s.map_us     = g_srMapUs.load(std::memory_order_relaxed);
    s.copy_bytes = g_srCopyBytes.load(std::memory_order_relaxed);
    s.copy_us    = g_srCopyUs.load(std::memory_order_relaxed);
    return s;
}

// Window size for every sliding-window reader. Overridable so the "is it the
// window churn?" question can be answered by a run, not by an argument.
size_t FS_StreamWindowSize()
{
    static size_t s_win = [] {
        size_t mb = 1;
        if (const char* e = std::getenv("XROS_FS_WINDOW_MB")) {
            const int v = atoi(e);
            if (v > 0 && v <= 512) mb = (size_t)v;
        }
        return mb * 1024 * 1024;
    }();
    return s_win;
}

void CMapStreamReader::map(const size_t& new_offset)
{
    CTimer _tMap;
    _tMap.Start();
    struct _mapScope {
        CTimer& t;
        ~_mapScope() {
            g_srMaps.fetch_add(1, std::memory_order_relaxed);
            g_srMapUs.fetch_add((u64)(t.GetElapsed_ms_total() * 1000.f), std::memory_order_relaxed);
        }
    } _ms{ _tMap };

    VERIFY(new_offset <= m_file_size);

    m_current_offset_from_start = new_offset;

    const size_t granularity = FS.dwAllocGranularity;
    size_t start_offset = m_start_offset + new_offset;
    const size_t pure_start_offset = start_offset;
    start_offset = (start_offset / granularity) * granularity;

    VERIFY(pure_start_offset >= start_offset);
    const size_t pure_end_offset = m_window_size + pure_start_offset;
    size_t end_offset = pure_end_offset / granularity;
    if (pure_end_offset % granularity)
        ++end_offset;

    end_offset *= granularity;
    if (end_offset > m_archive_size)
        end_offset = m_archive_size;

    m_current_window_size = end_offset - start_offset;
    m_current_map_view_of_file = static_cast<u8*>(MapViewOfFile(m_file_mapping_handle, FILE_MAP_READ, 0, start_offset, m_current_window_size));
    m_current_pointer = m_current_map_view_of_file;

    const size_t difference = pure_start_offset - start_offset;
    m_current_window_size -= difference;
    m_current_pointer += difference;
    m_start_pointer = m_current_pointer;
}

void CMapStreamReader::advance(std::ptrdiff_t offset)
{
    VERIFY(m_current_pointer >= m_start_pointer);
    std::ptrdiff_t offset_inside_window{static_cast<std::ptrdiff_t>(m_current_pointer - m_start_pointer)};
    VERIFY(offset_inside_window <= static_cast<std::ptrdiff_t>(m_current_window_size));
    if (offset_inside_window + offset >= static_cast<std::ptrdiff_t>(m_current_window_size))
    {
        remap(m_current_offset_from_start + offset_inside_window + offset);
        return;
    }

    if (offset_inside_window + offset < 0)
    {
        remap(m_current_offset_from_start + offset_inside_window + offset);
        return;
    }

    m_current_pointer += offset;
}

void CMapStreamReader::r(void* _buffer, size_t buffer_size)
{
    CTimer _tCopy;
    _tCopy.Start();
    const size_t _copyReq = buffer_size;
    struct _copyScope {
        CTimer& t; const size_t& n;
        ~_copyScope() {
            g_srCopyBytes.fetch_add((u64)n, std::memory_order_relaxed);
            g_srCopyUs.fetch_add((u64)(t.GetElapsed_ms_total() * 1000.f), std::memory_order_relaxed);
        }
    } _cs{ _tCopy, _copyReq };

    VERIFY(m_current_pointer >= m_start_pointer);
    VERIFY(size_t(m_current_pointer - m_start_pointer) <= m_current_window_size);


    int offset_inside_window = int(m_current_pointer - m_start_pointer);
    if (offset_inside_window + buffer_size < m_current_window_size)
    {
        Memory.mem_copy(_buffer, m_current_pointer, buffer_size);
        m_current_pointer += buffer_size;
        return;
    }

    u8* buffer = static_cast<u8*>(_buffer);
    size_t elapsed_in_window = size_t(m_current_window_size - (m_current_pointer - m_start_pointer));

    do
    {
        Memory.mem_copy(buffer, m_current_pointer, elapsed_in_window);
        buffer += elapsed_in_window;
        buffer_size -= elapsed_in_window;
        advance(elapsed_in_window);

        elapsed_in_window = m_current_window_size;
    } while (m_current_window_size < buffer_size);

    Memory.mem_copy(buffer, m_current_pointer, buffer_size);
    advance(buffer_size);
}

CStreamReader* CMapStreamReader::open_chunk(u32 chunk_id)
{
    BOOL compressed;
    const size_t size = find_chunk(chunk_id, &compressed);
    if (!size)
        return nullptr;

    R_ASSERT(!compressed, "cannot use CMapStreamReader on compressed chunks");

    CMapStreamReader* result = xr_new<CMapStreamReader>();
    result->construct(file_mapping_handle(), m_start_offset + tell(), size, m_archive_size, m_window_size);
    return (result);
}
