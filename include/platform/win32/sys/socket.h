// =============================================================================
// <sys/socket.h> 的 Windows 兼容实现（声明层）
//
// 用法：Windows 构建时本目录被前置到 include 路径，源文件中的
//   #include <sys/socket.h>
// 会解析到这里。POSIX 分支不受影响（该目录仅在 WIN32 下加入 include 路径）。
//
// 命名策略（关键）：
//   ws2_32 与 MSVC 运行库同样导出 socket/bind/accept/send/... 这些 C 符号，
//   如果我们在实现单元里定义同名符号，链接期会 "multiple definition"。
//   因此：实现单元只定义 tws_xxx，本头文件用 static inline 转发同名 POSIX 接口。
//   static inline 具备内部链接，既不与库符号冲突，也不需要额外链接配置。
//
// 语义约定（与 Linux 的差异都写在这里，不藏在实现里）：
//   * fd 不是原生 SOCKET，而是"句柄表索引"（见 src/platform/win32_compat.cpp），
//     因此可以安全地放在 int 里，并可与 CRT 文件描述符区分。
//   * SOCK_NONBLOCK / SOCK_CLOEXEC 在 socket()/accept4() 中被解释为"创建后置非阻塞"。
//   * SO_REUSEPORT 在 Windows 上不受支持：setsockopt 返回 -1/ENOPROTOOPT，
//     使现有 "不支持则降级/跳过" 的逻辑自然生效（显式降级，不静默假装成功）。
//   * EPOLLET（边缘触发）语义由 <sys/epoll.h> 的 LT 兼容层承担。
// =============================================================================
#ifndef TWS_WIN32_SYS_SOCKET_H
#define TWS_WIN32_SYS_SOCKET_H

#include "tws_posix_types.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>

// ---- 常量（取值沿用 Linux，便于阅读；仅在本层内部解释） ----------------------
#define AF_UNSPEC   0
#define AF_INET     2
#define AF_INET6    10
#define PF_INET     AF_INET

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC  0x80000

#define SOL_SOCKET    1
#define IPPROTO_TCP   6
#define IPPROTO_IP    0

#define SO_REUSEADDR  2
#define SO_ERROR      4
#define SO_REUSEPORT  15
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21
#define SO_KEEPALIVE  9
#define SO_LINGER     13

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define MSG_NOSIGNAL  0x4000
#define MSG_DONTWAIT  0x0040
#define MSG_PEEK      0x0002

// ---- 地址结构（布局与 winsock 的 sockaddr_in 一致，可安全按指针互转） --------
struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in {
    short          sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char        __data[126];
};

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)

// ---- 实现（src/platform/win32_compat.cpp） ----------------------------------
extern "C" {
int     tws_socket(int domain, int type, int protocol);
int     tws_bind(int fd, const struct sockaddr* addr, socklen_t len);
int     tws_listen(int fd, int backlog);
int     tws_accept(int fd, struct sockaddr* addr, socklen_t* len);
int     tws_connect(int fd, const struct sockaddr* addr, socklen_t len);
ssize_t tws_send(int fd, const void* buf, size_t len, int flags);
ssize_t tws_recv(int fd, void* buf, size_t len, int flags);
ssize_t tws_sendto(int fd, const void* buf, size_t len, int flags,
                   const struct sockaddr* addr, socklen_t addrlen);
int     tws_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen);
int     tws_getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen);
int     tws_getsockname(int fd, struct sockaddr* addr, socklen_t* len);
int     tws_getpeername(int fd, struct sockaddr* addr, socklen_t* len);
int     tws_shutdown(int fd, int how);
}

// accept4 在 ws2_32 中不存在同名符号，可直接定义
extern "C" int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags);

// ---- 同名 POSIX 转发（static inline = 内部链接，不与 ws2_32 冲突） -----------
static inline int socket(int domain, int type, int protocol) {
    return tws_socket(domain, type, protocol);
}
static inline int bind(int fd, const struct sockaddr* addr, socklen_t len) {
    return tws_bind(fd, addr, len);
}
static inline int listen(int fd, int backlog) { return tws_listen(fd, backlog); }
static inline int accept(int fd, struct sockaddr* addr, socklen_t* len) {
    return tws_accept(fd, addr, len);
}
static inline int connect(int fd, const struct sockaddr* addr, socklen_t len) {
    return tws_connect(fd, addr, len);
}
static inline ssize_t send(int fd, const void* buf, size_t len, int flags) {
    return tws_send(fd, buf, len, flags);
}
static inline ssize_t recv(int fd, void* buf, size_t len, int flags) {
    return tws_recv(fd, buf, len, flags);
}
static inline ssize_t sendto(int fd, const void* buf, size_t len, int flags,
                             const struct sockaddr* addr, socklen_t addrlen) {
    return tws_sendto(fd, buf, len, flags, addr, addrlen);
}
static inline int setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen) {
    return tws_setsockopt(fd, level, optname, optval, optlen);
}
static inline int getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen) {
    return tws_getsockopt(fd, level, optname, optval, optlen);
}
static inline int getsockname(int fd, struct sockaddr* addr, socklen_t* len) {
    return tws_getsockname(fd, addr, len);
}
static inline int getpeername(int fd, struct sockaddr* addr, socklen_t* len) {
    return tws_getpeername(fd, addr, len);
}
static inline int shutdown(int fd, int how) { return tws_shutdown(fd, how); }

#endif  // TWS_WIN32_SYS_SOCKET_H
