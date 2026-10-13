# Linux build of the vendored libsquashfs (squashfs-tools-ng, library part only). Source list
# copied from libsquashfs.vcxproj — keep in sync — with the two Win32 I/O backends swapped
# for their POSIX counterparts from the same tree (io/win32.c -> io/unix.c,
# io/dir_win32.c -> io/dir_unix.c). Not using the upstream autotools build: it wants the
# whole toolchain (and optional zlib/xz/lzo/selinux probing) for what is 52 C files here.
cmake_minimum_required(VERSION 3.18)
project(libsquashfs C)

set(S ${CMAKE_CURRENT_SOURCE_DIR}/squashfs-tools-ng/lib)
add_library(squashfs STATIC
    ${S}/compat/src/strchrnul.c ${S}/compat/src/strndup.c
    ${S}/sqfs/src/block_processor/backend.c ${S}/sqfs/src/block_processor/block_processor.c
    ${S}/sqfs/src/block_processor/frontend.c ${S}/sqfs/src/block_processor/ostream.c
    ${S}/sqfs/src/block_writer.c
    ${S}/sqfs/src/comp/compressor.c ${S}/sqfs/src/comp/lz4.c ${S}/sqfs/src/comp/zstd.c
    ${S}/sqfs/src/data_reader.c ${S}/sqfs/src/dir_entry.c ${S}/sqfs/src/dir_iterator.c
    ${S}/sqfs/src/dir_reader.c ${S}/sqfs/src/dir_writer.c ${S}/sqfs/src/frag_table.c
    ${S}/sqfs/src/id_table.c ${S}/sqfs/src/inode.c
    ${S}/sqfs/src/io/dir_hl.c ${S}/sqfs/src/io/dir_rec.c
    ${S}/sqfs/src/io/dir_unix.c   # vcxproj: dir_win32.c
    ${S}/sqfs/src/io/file.c ${S}/sqfs/src/io/istream.c ${S}/sqfs/src/io/ostream.c
    ${S}/sqfs/src/io/stream_api.c
    ${S}/sqfs/src/io/unix.c       # vcxproj: win32.c
    ${S}/sqfs/src/meta_reader.c ${S}/sqfs/src/meta_writer.c ${S}/sqfs/src/misc.c
    ${S}/sqfs/src/read_inode.c ${S}/sqfs/src/read_super.c ${S}/sqfs/src/read_table.c
    ${S}/sqfs/src/readdir.c ${S}/sqfs/src/super.c ${S}/sqfs/src/write_inode.c
    ${S}/sqfs/src/write_super.c ${S}/sqfs/src/write_table.c
    ${S}/sqfs/src/xattr/xattr.c ${S}/sqfs/src/xattr/xattr_reader.c ${S}/sqfs/src/xattr/xattr_writer.c
    ${S}/sqfs/src/xattr/xattr_writer_flush.c ${S}/sqfs/src/xattr/xattr_writer_record.c
    ${S}/util/src/alloc.c ${S}/util/src/array.c ${S}/util/src/file_cmp.c ${S}/util/src/hash_table.c
    ${S}/util/src/is_memory_zero.c ${S}/util/src/mempool.c ${S}/util/src/rbtree.c
    ${S}/util/src/str_table.c ${S}/util/src/threadpool.c ${S}/util/src/xxhash.c
)
target_include_directories(squashfs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/squashfs-tools-ng/include
)
target_include_directories(squashfs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc_posix
)
if(OGSR_SWITCH)
    # newlib: <sys/endian.h> only; forward glibc's <endian.h> to it.
    target_include_directories(squashfs PRIVATE ${OGSR_ENGINE_DIR}/cmake/nx_compat)
endif()
target_compile_definitions(squashfs PUBLIC SQFS_STATIC)
target_compile_definitions(squashfs PRIVATE NDEBUG WITH_LZ4 WITH_ZSTD _GNU_SOURCE)
find_package(Threads REQUIRED)
target_link_libraries(squashfs PUBLIC lz4 zstd Threads::Threads)
