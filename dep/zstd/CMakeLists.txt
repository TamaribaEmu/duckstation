# Zstandard 1.5.6 (lib/common, lib/compress, lib/decompress from the release tarball), for the libretro core, which
# takes no libraries from the system but zlib. Built without the x86-64 assembly decoder.
file(GLOB ZSTD_SRCS lib/common/*.c lib/compress/*.c lib/decompress/*.c)
add_library(zstd STATIC ${ZSTD_SRCS})
target_include_directories(zstd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/lib")
target_compile_definitions(zstd PRIVATE ZSTD_DISABLE_ASM=1)
add_library(Zstd::Zstd ALIAS zstd)
