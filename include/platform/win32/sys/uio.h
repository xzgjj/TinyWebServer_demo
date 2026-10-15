// <sys/uio.h> 的 Windows 兼容实现：分散/聚集 I/O。
// iovec 布局与 Linux 一致（实现层翻译成 WSABUF）。
#ifndef TWS_WIN32_SYS_UIO_H
#define TWS_WIN32_SYS_UIO_H

#include "tws_posix_types.h"

#include <cstddef>

struct iovec {
    void*  iov_base;
    size_t iov_len;
};

extern "C" {
ssize_t readv(int fd, const struct iovec* iov, int iovcnt);
ssize_t writev(int fd, const struct iovec* iov, int iovcnt);
}

#endif  // TWS_WIN32_SYS_UIO_H
