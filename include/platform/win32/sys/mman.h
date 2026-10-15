// <sys/mman.h> 的 Windows 兼容实现：只读文件映射（静态资源零拷贝路径使用）。
// 实现走 CreateFileMapping/MapViewOfFile（见 src/platform/win32_filemap.cpp）。
#ifndef TWS_WIN32_SYS_MMAN_H
#define TWS_WIN32_SYS_MMAN_H

#include "tws_posix_types.h"

#include <cstddef>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2

#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED  ((void*)-1)

extern "C" {
void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset);
int   munmap(void* addr, size_t length);
}

#endif  // TWS_WIN32_SYS_MMAN_H
