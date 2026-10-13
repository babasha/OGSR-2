# Linux build of the vendored zstd. Source list copied from libzstd.vcxproj — keep in sync.
# (zstd's own build/cmake is not used: it builds programs/tests and a shared lib by default,
# and this keeps the file set identical to what MSVC compiles.)
cmake_minimum_required(VERSION 3.18)
project(zstd C)

set(Z ${CMAKE_CURRENT_SOURCE_DIR}/zstd/lib)
add_library(zstd STATIC
    ${Z}/common/pool.c ${Z}/common/debug.c ${Z}/common/threading.c ${Z}/common/entropy_common.c
    ${Z}/common/error_private.c ${Z}/common/xxhash.c ${Z}/common/zstd_common.c ${Z}/common/fse_decompress.c
    ${Z}/compress/hist.c ${Z}/compress/fse_compress.c ${Z}/compress/huf_compress.c ${Z}/compress/zstd_compress.c
    ${Z}/compress/zstd_compress_literals.c ${Z}/compress/zstd_compress_sequences.c
    ${Z}/compress/zstd_compress_superblock.c ${Z}/compress/zstd_preSplit.c ${Z}/compress/zstd_fast.c
    ${Z}/compress/zstd_double_fast.c ${Z}/compress/zstd_lazy.c ${Z}/compress/zstd_opt.c ${Z}/compress/zstd_ldm.c
    ${Z}/compress/zstdmt_compress.c
    ${Z}/decompress/huf_decompress.c ${Z}/decompress/zstd_decompress.c ${Z}/decompress/zstd_decompress_block.c
    ${Z}/decompress/zstd_ddict.c
    ${Z}/dictBuilder/cover.c ${Z}/dictBuilder/fastcover.c ${Z}/dictBuilder/divsufsort.c ${Z}/dictBuilder/zdict.c
    ${Z}/legacy/zstd_v01.c ${Z}/legacy/zstd_v02.c ${Z}/legacy/zstd_v03.c ${Z}/legacy/zstd_v04.c
    ${Z}/legacy/zstd_v05.c ${Z}/legacy/zstd_v06.c ${Z}/legacy/zstd_v07.c
)
target_include_directories(zstd PUBLIC ${Z})
target_compile_definitions(zstd PRIVATE
    NDEBUG
    ZSTD_MULTITHREAD=1
    ZSTD_LEGACY_SUPPORT=5
    # MSVC never builds huf_decompress_amd64.S; keep Linux on the same (portable C) decoder.
    # Also the only option on arm64.
    ZSTD_DISABLE_ASM
)
find_package(Threads REQUIRED)
target_link_libraries(zstd PUBLIC Threads::Threads)
