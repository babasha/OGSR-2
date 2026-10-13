#pragma once

class XRCORE_API CStreamReader : public IReaderBase<CStreamReader>
{
public:
    virtual size_t elapsed() const = 0;
    virtual size_t length() const = 0;
    virtual void seek(std::ptrdiff_t offset) = 0;
    virtual size_t tell() const = 0;
    virtual void close() = 0;

    virtual void advance(std::ptrdiff_t offset) = 0;
    virtual void r(void* buffer, size_t buffer_size) = 0;
    virtual CStreamReader* open_chunk(u32 chunk_id) = 0;

    // The bytes the caller can consume IN PLACE, without r() copying them out
    // first: a pointer into the mapped window and how much is left in it before
    // a remap. A reader that does not map (archive entries) answers {null, 0}
    // and the caller keeps using r().
    // Absolute offset, in the underlying file or archive, of the next byte this
    // reader will hand out -- or size_t(-1) when it cannot say. Lets a caller that
    // holds ANOTHER view of the same file stage out of that one instead (the
    // geometry loader does: a fresh window mapping costs a fault per 4 KB).
    virtual size_t abs_pos() const { return size_t(-1); }

    virtual const u8* window_pointer() const { return nullptr; }
    virtual size_t window_avail() const { return 0; }
};

template <>
struct std::default_delete<CStreamReader>
{
    _CONSTEXPR23 void operator()(CStreamReader* ptr) const noexcept { ptr->close(); }
};

class CMapStreamReader : public CStreamReader
{
private:
    HANDLE m_file_mapping_handle{};
    size_t m_start_offset{};
    size_t m_file_size{};
    size_t m_archive_size{};
    size_t m_window_size{};

private:
    size_t m_current_offset_from_start{};
    size_t m_current_window_size{};
    u8* m_current_map_view_of_file{};
    u8* m_start_pointer{};
    u8* m_current_pointer{};

private:
    void map(const size_t& new_offset);
    IC void unmap();
    IC void remap(const size_t& new_offset);

private:
    // should not be called
    CMapStreamReader(const CMapStreamReader& object) = delete;
    CMapStreamReader& operator=(const CMapStreamReader&) = delete;

public:
    IC CMapStreamReader();

public:
    virtual void construct(const HANDLE& file_mapping_handle, const size_t& start_offset, const size_t& file_size, const size_t& archive_size, const size_t& window_size);
    virtual void destroy();

public:
    IC const HANDLE& file_mapping_handle() const;

    IC size_t elapsed() const override;
    IC size_t length() const override;
    IC void seek(std::ptrdiff_t offset) override;
    IC size_t tell() const override;
    IC void close() override;

    void advance(std::ptrdiff_t offset) override;
    void r(void* buffer, size_t buffer_size) override;
    CStreamReader* open_chunk(u32 chunk_id) override;

    size_t abs_pos() const override { return m_start_offset + m_current_offset_from_start + size_t(m_current_pointer - m_start_pointer); }
    const u8* window_pointer() const override { return m_current_pointer; }
    size_t window_avail() const override { return size_t(m_current_window_size - (m_current_pointer - m_start_pointer)); }
};

#include "stream_reader_inline.h"
