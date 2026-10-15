// =============================================================================
// <sys/epoll.h> 的 Windows 兼容实现
//
// 语义声明（重要，不允许含糊）：
//   * 本实现基于 WSAPoll，是【水平触发(LT)】语义。Linux 侧的 EPOLLET 标志被
//     接受但不改变行为。
//   * 为什么 LT 对本项目是安全的：项目按 ET 纪律编写（accept/read/write 都循环
//     直到 EAGAIN）。这类代码在 LT 下【结果正确】，只是可能多一些空转唤醒。
//     反过来（LT 代码跑在 ET 下）才会丢事件 —— 本项目不属于这种情况。
//   * EPOLLONESHOT 语义按"一次性"实现：触发后自动从关注集合移除，需要 MOD 重新武装
//     （本项目 EventLoop::UpdateEvent 每次都会 MOD，因此行为一致）。
// =============================================================================
#ifndef TWS_WIN32_SYS_EPOLL_H
#define TWS_WIN32_SYS_EPOLL_H

#include "tws_posix_types.h"

#include <cstdint>

#define EPOLLIN      0x0001
#define EPOLLPRI     0x0002
#define EPOLLOUT     0x0004
#define EPOLLERR     0x0008
#define EPOLLHUP     0x0010
#define EPOLLRDHUP   0x2000
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP   (1u << 29)
#define EPOLLONESHOT  (1u << 30)
#define EPOLLET       (1u << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC  0x80000
#define EPOLL_NONBLOCK 0x800

typedef union epoll_data {
    void*    ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct __attribute__((packed)) epoll_event {
    uint32_t     events;
    epoll_data_t data;
};

extern "C" {
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout);
}

#endif  // TWS_WIN32_SYS_EPOLL_H
