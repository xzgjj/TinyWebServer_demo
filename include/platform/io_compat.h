// =============================================================================
// 跨平台 fd I/O 兼容层
//
// 背景：Windows 上 socket 不是 CRT 文件描述符，::read/::write/::close 对 socket
// 无效（会返回 EBADF 或误伤同号文件句柄）。因此服务端代码统一通过本层做 fd I/O。
//
// 约束：
//   * 不使用宏重定义 read/write/close —— 那样会误伤 std::bind、std::cout.write、
//     ofstream::close 等成员函数调用。这里只用显式函数调用。
//   * POSIX 分支是零成本的 inline 转发（行为与改造前完全一致）。
// =============================================================================
#ifndef TWS_PLATFORM_IO_COMPAT_H
#define TWS_PLATFORM_IO_COMPAT_H

#include <cstddef>

#ifdef TWS_PLATFORM_WIN32
#include <sys/types.h>

extern "C" {
int     tws_io_close(int fd);
ssize_t tws_io_read(int fd, void* buf, size_t count);
ssize_t tws_io_write(int fd, const void* buf, size_t count);
int     tws_io_shutdown(int fd, int how);
int     tws_io_set_nonblocking(int fd, int on);
int     tws_io_is_managed(int fd);
}
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tws {

/// 关闭 fd：对 socket 与普通文件都能正确处理。
inline int close_fd(int fd) {
#ifdef TWS_PLATFORM_WIN32
    return tws_io_close(fd);
#else
    return ::close(fd);
#endif
}

inline ssize_t read_fd(int fd, void* buf, std::size_t count) {
#ifdef TWS_PLATFORM_WIN32
    return tws_io_read(fd, buf, count);
#else
    return ::read(fd, buf, count);
#endif
}

inline ssize_t write_fd(int fd, const void* buf, std::size_t count) {
#ifdef TWS_PLATFORM_WIN32
    return tws_io_write(fd, buf, count);
#else
    return ::write(fd, buf, count);
#endif
}

inline int shutdown_fd(int fd, int how) {
#ifdef TWS_PLATFORM_WIN32
    return tws_io_shutdown(fd, how);
#else
    return ::shutdown(fd, how);
#endif
}

/// 把 fd 置为非阻塞（socket 用 ioctlsocket，普通 fd 用 fcntl）。
inline int set_nonblocking_fd(int fd, bool on) {
#ifdef TWS_PLATFORM_WIN32
    return tws_io_set_nonblocking(fd, on ? 1 : 0);
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (on) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return ::fcntl(fd, F_SETFL, flags);
#endif
}

/// 该 fd 是否由本兼容层托管（Windows 上为句柄表索引）。
inline bool is_managed_fd(int fd) {
#ifdef TWS_PLATFORM_WIN32
    return tws_io_is_managed(fd) != 0;
#else
    (void)fd;
    return true;
#endif
}

}  // namespace tws

#endif  // TWS_PLATFORM_IO_COMPAT_H
