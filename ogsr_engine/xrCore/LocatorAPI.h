// LocatorAPI.h: interface for the CLocatorAPI class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

// ⚠19-09: ничего из <io.h> (_access/_open/_findfirst и т.п.) в этом модуле
// не вызывается (грепом проверено) -- включение мёртвое, оставлено
// MSVC-only вместо портирования вникуда.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4995)
#include <io.h>
#pragma warning(pop)
#endif

#include "LocatorAPI_defs.h"

class XRCORE_API CStreamReader;

namespace sqfs
{
struct sqfs_dir_iterator_t;
struct sqfs_file_t;
} // namespace sqfs

// ---------------------------------------------------------------------------
// Where opened files actually COME FROM. A level load spends seconds inside
// r_open, and the three sources have completely different fixes: a loose file is
// three cheap syscalls plus lazy paging, an uncompressed archive entry is one
// MapViewOfFile, and a compressed one is a whole-file LZO decompress on the
// CALLING thread. Counting them apart is the only way to know which one is the
// cost. Snapshot before/after a phase and subtract.
// ---------------------------------------------------------------------------
// Sliding-window reader accounting (CMapStreamReader). `map_us` is the
// MapViewOfFile/Unmap churn, `copy_us` the memcpy out of the window — which is
// also where the page faults land, so a low MB/s here means paging, not copying.
struct FS_StreamStats
{
    u64 maps{}, map_us{};
    u64 copy_bytes{}, copy_us{};
};
XRCORE_API FS_StreamStats FS_GetStreamStats();

// Window size used by every sliding-window reader (default 1 MB, overridable
// with XROS_FS_WINDOW_MB).
XRCORE_API size_t FS_StreamWindowSize();

struct FS_OpenStats
{
    u64 loose_n{},  loose_b{},  loose_us{};    // on-disk file    -> CreateFile + MapViewOfFile
    u64 packed_n{}, packed_b{}, packed_us{};   // archive, stored -> MapViewOfFile
    u64 compr_n{},  compr_b{},  compr_us{};    // archive, LZO    -> alloc + decompress
};
XRCORE_API FS_OpenStats FS_GetOpenStats();

class XRCORE_API CLocatorAPI
{
    friend class FS_Path;

public:
    struct file
    {
        LPCSTR name; // low-case name
        size_t vfs; // VFS_STANDARD_FILE - standart file

        u32 size_real;
        u32 folder : 1;

        union
        {
            // SquashFS
            u64 inode;

            // DB
            struct
            {
                u32 ptr; // pointer inside vfs
                u32 size_compressed; // if (size_real==size_compressed) - uncompressed
            };
        };

        u32 time_write; // for editor

        shared_str real_file_path;
    };

private:
    static constexpr size_t VFS_STANDARD_FILE = std::numeric_limits<size_t>::max();
    // Superseded by FS_StreamWindowSize() so the window can be A/B-ed at runtime.
    static constexpr size_t BIG_FILE_READER_WINDOW_SIZE = 1024 * 1024;

    DEFINE_MAP_PRED(LPCSTR, FS_Path*, PathMap, PathPairIt, pred_str)
    PathMap pathes;

    void rescan_physical_path(LPCSTR full_path, BOOL bRecurse);

    struct file_pred
    {
        IC bool operator()(const file& x, const file& y) const { return xr_strcmp(x.name, y.name) < 0; }
    };

    DEFINE_SET_PRED(file, files_set, files_it, file_pred)
    files_set files;

    struct archive
    {
        shared_str path{};
        size_t vfs_idx{VFS_STANDARD_FILE};
        size_t size{};

    private:
        class xr_sqfs;
        class xr_sqfs_stream;

        enum container : u32
        {
            STANDARD,
            SQFS,
            DB,
        } type{};

        // SquashFS
        xr_sqfs* fs{};

        // DB
        void* hSrcMap{};

        void* hSrcFile{};

    public:
        archive() = default;

        // Implementation wrappers
        void open();

        IC void index(CLocatorAPI& loc, const char* fs_entry_point);
        IC IReader* read(const char* fname, const struct file& desc, u32 gran);
        IC CStreamReader* stream(const char* fname, const struct file& desc);
        IC void cleanup();
        void close();

    private:
        // SquashFS
        void open_sqfs();

        void index_dir_sqfs(CLocatorAPI& loc, const char* path, sqfs::sqfs_dir_iterator_t& it) const;
        void index_sqfs(CLocatorAPI& loc, const char* fs_entry_point) const;
        IReader* read_sqfs(const char* fname, const struct file& desc, u32 gran) const;
        CStreamReader* stream_sqfs(const char* fname, const struct file& desc) const;
        void cleanup_sqfs();
        void close_sqfs();

        // DB
        void open_db();

        void index_db(CLocatorAPI& loc, const char* entry_point) const;
        IReader* read_db(const char* fname, const struct file& desc, u32 gran) const;
        CStreamReader* stream_db(const char* fname, const struct file& desc) const;
        void cleanup_db();
        void close_db();
    };

    DEFINE_VECTOR(archive, archives_vec, archives_it)
    archives_vec archives;

    void Register(LPCSTR name, size_t vfs, u32 ptr, u32 size_real, u32 size_compressed, u32 time_write, bool folder = false);
    void RegisterFolderHierarchy(LPCSTR folder_path);

    void LoadArchive(archive& A, LPCSTR base_path);
    void ProcessArchive(LPCSTR path, LPCSTR base_path = nullptr);
    void ProcessOne(LPCSTR path, const _finddata_t& F, bool bNoRecurse);
    bool RecurseScanPhysicalPath(const char* path, const bool log_if_found, bool bNoRecurse);

    files_it file_find_it(LPCSTR fname);

    CInifile* gamedata_unused_references{};

public:
    enum
    {
        // = (1 << 0),
        // = (1 << 1),
        flReady = (1 << 2),
        // = (1 << 3),
        // = (1 << 4),
        // = (1 << 5),
        // = (1 << 6),
        // = (1 << 7),
        flNeedCheck = (1 << 8),
        flDumpFileActivity = (1 << 9),
    };
    Flags32 m_Flags;
    u32 dwAllocGranularity;
    u32 dwOpenCounter;

private:
    void file_from_cache_impl(IReader*& R, LPCSTR fname, const file& desc);
    void file_from_cache_impl(CStreamReader*& R, LPCSTR fname, const file& desc);

    template <typename T>
    void file_from_cache(T*& R, LPCSTR fname, const file& desc);

    void file_from_archive(IReader*& R, LPCSTR fname, const file& desc);
    void file_from_archive(CStreamReader*& R, LPCSTR fname, const file& desc);

    bool check_for_file(LPCSTR path, LPCSTR _fname, string_path& fname_result, const file*& desc);

    template <typename T>
    IC T* r_open_impl(LPCSTR path, LPCSTR _fname);

    void ProcessExternalMods(LPCSTR base_path);

    void free_file(const file& f);

public:
    CLocatorAPI();
    ~CLocatorAPI();
    void _initialize(u32 flags, LPCSTR fs_name = nullptr);
    void _destroy();

    CStreamReader* rs_open(LPCSTR initial, LPCSTR N);
    IReader* r_open(LPCSTR initial, LPCSTR N);
    IC IReader* r_open(LPCSTR N) { return r_open(nullptr, N); }
    // A small file read again on every event (the camera/pp effector of each hit
    // on the player): read once, then a reader over the kept bytes. r_close as usual.
    IReader* r_open_cached(LPCSTR full_path);
    void r_close(IReader*& S);
    void r_close(CStreamReader*& fs);

    IWriter* w_open(LPCSTR initial, LPCSTR N);
    IC IWriter* w_open(LPCSTR N) { return w_open(nullptr, N); }
    IWriter* w_open_ex(LPCSTR initial, LPCSTR N);
    IC IWriter* w_open_ex(LPCSTR N) { return w_open_ex(nullptr, N); }
    void w_close(IWriter*& S);

    const file* exist(LPCSTR N);
    const file* exist(LPCSTR path, LPCSTR name);
    const file* exist(string_path& fn, LPCSTR path, LPCSTR name);
    const file* exist(string_path& fn, LPCSTR path, LPCSTR name, LPCSTR ext);
    // every known file whose full name starts with full_prefix (lowercase, as update_path makes it): one ordered walk
    // instead of an exist() per guessed name
    void file_find_prefix(LPCSTR full_prefix, xr_vector<LPCSTR>& out);

    void file_delete(LPCSTR path, LPCSTR nm);
    void file_delete(LPCSTR full_path) { file_delete(nullptr, full_path); }
    void file_copy(LPCSTR src, LPCSTR dest);

    u32 get_file_age(LPCSTR nm);

    xr_vector<LPSTR>* file_list_open(const char* initial, const char* folder, u32 flags = FS_ListFiles);
    xr_vector<LPSTR>* file_list_open(const char* path, u32 flags = FS_ListFiles);
    void file_list_close(xr_vector<LPSTR>*& lst);

    bool path_exist(LPCSTR path);
    FS_Path* get_path(LPCSTR path);
    FS_Path* append_path(LPCSTR path_alias, LPCSTR root, LPCSTR add, BOOL recursive);
    LPCSTR update_path(string_path& dest, LPCSTR initial, LPCSTR src);

    int file_list(FS_FileSet& dest, LPCSTR path, u32 flags = FS_ListFiles, LPCSTR mask = nullptr);

    void rescan_physical_pathes();

    // debug
    void init_gamedata_unused();
};

extern XRCORE_API std::unique_ptr<CLocatorAPI> xr_FS;
#define FS (*xr_FS)
