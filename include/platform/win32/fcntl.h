// <fcntl.h> 的 Windows 兼容实现：保留 MinGW 的定义，补齐 POSIX 侧用到的常量与
// fcntl() 入口（本项目用它把 socket 置为非阻塞）。
// 说明：这里用 #include_next 继续搜索后续目录，绝不覆盖 MinGW 的原生定义。
#ifndef TWS_WIN32_FCNTL_H
#define TWS_WIN32_FCNTL_H

#include_next <fcntl.h>

#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x40000
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif

extern "C" {
int fcntl(int fd, int cmd, ...);
}

#endif  // TWS_WIN32_FCNTL_H
