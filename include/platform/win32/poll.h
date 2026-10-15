// <poll.h> 的 Windows 兼容实现（基于 WSAPoll；fd 为本层句柄表索引）。
#ifndef TWS_WIN32_POLL_H
#define TWS_WIN32_POLL_H

#include "tws_posix_types.h"

#define POLLIN     0x0001
#define POLLPRI    0x0002
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020
#define POLLRDNORM 0x0040
#define POLLWRNORM 0x0100

struct pollfd {
    int   fd;
    short events;
    short revents;
};

extern "C" {
int poll(struct pollfd* fds, unsigned long nfds, int timeout);
}

#endif  // TWS_WIN32_POLL_H
