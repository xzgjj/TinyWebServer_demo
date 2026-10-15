// <sys/eventfd.h> 的 Windows 兼容实现。
// 实现用一个"自连 UDP 套接字"充当唤醒通道：write() 发送 8 字节计数，
// read() 取回并清零，可被 WSAPoll/epoll_wait 正常等待。
#ifndef TWS_WIN32_SYS_EVENTFD_H
#define TWS_WIN32_SYS_EVENTFD_H

#include "tws_posix_types.h"

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   0x80000
#define EFD_NONBLOCK  0x800

extern "C" {
int eventfd(unsigned int initval, int flags);
}

#endif  // TWS_WIN32_SYS_EVENTFD_H
