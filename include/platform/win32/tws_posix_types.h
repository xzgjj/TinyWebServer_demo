// =============================================================================
// Win32 兼容层：POSIX 基础类型（不引入 winsock/windows 头，避免与源文件里的
// POSIX 同名结构冲突）。
//
// 为什么单独一个文件：
//   源文件按 POSIX 习惯书写（sockaddr_in / iovec / epoll_event ...）。Windows 上
//   这些名字或者不存在，或者由 winsock 定义成另一套 ABI。本层提供【布局与 winsock
//   完全一致】的自有定义；实现单元（win32_compat.cpp）只依赖本层，从而做到
//   "实现里没有 windows 类型、源文件里没有 winsock 类型"。
// =============================================================================
#ifndef TWS_WIN32_POSIX_TYPES_H
#define TWS_WIN32_POSIX_TYPES_H

#include <cstddef>
#include <cstdint>
#include <sys/types.h>   // MinGW 提供（crtdefs.h），不含 winsock

#ifndef _SSIZE_T_DEFINED
typedef long long ssize_t;
#endif

// MinGW 的 _mingw.h 用同一宏保护 struct timeval；这里保持一致的布局。
#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

typedef unsigned short sa_family_t;
typedef uint32_t       in_addr_t;
typedef uint16_t       in_port_t;
typedef int            socklen_t;

#define TWS_WIN32 1

#endif  // TWS_WIN32_POSIX_TYPES_H
