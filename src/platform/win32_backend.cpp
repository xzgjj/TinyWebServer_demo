// =============================================================================
// Win32 后端实现（本 TU 是唯一接触 winsock 的地方）
//
// 职责：把 winsock 的调用、错误码、事件掩码翻译成"中立标量"，
//       供 POSIX ABI 层（win32_compat.cpp）使用。
// =============================================================================
#include "platform/win32_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>

#include <cerrno>
#include <cstdint>
#include <mutex>

namespace tws {
namespace win32 {
namespace {

std::once_flag g_once;
int g_last_error = 0;

void ensure_init() {
    std::call_once(g_once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

int map_wsa_error(int code) {
    switch (code) {
        case 0:                    return 0;
        case WSAEWOULDBLOCK:       return EWOULDBLOCK;
        case WSAEINPROGRESS:       return EINPROGRESS;
        case WSAEALREADY:          return EALREADY;
        case WSAECONNRESET:        return ECONNRESET;
        case WSAECONNABORTED:      return ECONNABORTED;
        case WSAECONNREFUSED:      return ECONNREFUSED;
        case WSAETIMEDOUT:         return ETIMEDOUT;
        case WSAEADDRINUSE:        return EADDRINUSE;
        case WSAEADDRNOTAVAIL:     return EADDRNOTAVAIL;
        case WSAENETUNREACH:       return ENETUNREACH;
        case WSAENETDOWN:          return ENETDOWN;
        case WSAENOTCONN:          return ENOTCONN;
        case WSAEISCONN:           return EISCONN;
        case WSAEINVAL:            return EINVAL;
        case WSAENOTSOCK:          return ENOTSOCK;
        case WSAEBADF:             return EBADF;
        case WSAEACCES:            return EACCES;
        case WSAENOBUFS:           return ENOBUFS;
        case WSAEMFILE:            return EMFILE;
        case WSAEMSGSIZE:          return EMSGSIZE;
        case WSAEOPNOTSUPP:        return EOPNOTSUPP;
        default:                   return EINVAL;
    }
}

int record_error() {
    g_last_error = map_wsa_error(WSAGetLastError());
    return g_last_error;
}

int record_success() {
    g_last_error = 0;
    return 0;
}

// 事件掩码翻译：本层对外统一使用"自定义 POLL/EPOLL 数值"（见 poll.h / sys/epoll.h），
// winsock 使用 POLLRDNORM/POLLWRNORM 等不同数值。
short to_wsa_events(short ours) {
    short out = 0;
    if (ours & 0x0001) { out |= POLLRDNORM; }   // POLLIN / EPOLLIN
    if (ours & 0x0004) { out |= POLLWRNORM; }   // POLLOUT / EPOLLOUT
    if (ours & 0x0002) { out |= POLLPRI; }      // POLLPRI / EPOLLPRI
    return out;
}

short from_wsa_events(short wsa) {
    short out = 0;
    if (wsa & (POLLRDNORM | POLLRDBAND | POLLIN)) { out |= 0x0001; }
    if (wsa & (POLLWRNORM | POLLWRBAND | POLLOUT)) { out |= 0x0004; }
    if (wsa & POLLPRI) { out |= 0x0002; }
    if (wsa & POLLERR) { out |= 0x0008; }
    if (wsa & POLLHUP) { out |= 0x0010; }
    if (wsa & POLLNVAL) { out |= 0x0020; }
    return out;
}

constexpr int kOurSoReuseAddr = 2;
constexpr int kOurSoError     = 4;
constexpr int kOurSoKeepAlive = 9;
constexpr int kOurSoReusePort = 15;
constexpr int kOurSoRcvTimeo  = 20;
constexpr int kOurSoSndTimeo  = 21;
constexpr int kOurTcpNoDelay  = 1;
constexpr int kOurTcpCork     = 3;

}  // namespace

void backend_init() { ensure_init(); }

int last_error() { return g_last_error; }

bool last_error_would_block() {
    return g_last_error == EWOULDBLOCK || g_last_error == EINPROGRESS || g_last_error == EAGAIN;
}

uint64_t sock_create(bool stream, bool nonblocking) {
    ensure_init();
    SOCKET s = ::socket(AF_INET, stream ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) {
        record_error();
        return 0;
    }
    if (nonblocking) {
        u_long mode = 1;
        if (::ioctlsocket(s, FIONBIO, &mode) != 0) {
            record_error();
            ::closesocket(s);
            return 0;
        }
    }
    record_success();
    return static_cast<uint64_t>(s);
}

int sock_bind(uint64_t s, uint32_t ip_be, uint16_t port_be) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip_be;
    addr.sin_port = port_be;
    if (::bind(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return record_error();
    }
    return record_success();
}

int sock_listen(uint64_t s, int backlog) {
    if (::listen(static_cast<SOCKET>(s), backlog) != 0) {
        return record_error();
    }
    return record_success();
}

uint64_t sock_accept(uint64_t s, bool nonblocking) {
    SOCKET c = ::accept(static_cast<SOCKET>(s), nullptr, nullptr);
    if (c == INVALID_SOCKET) {
        record_error();
        return 0;
    }
    if (nonblocking) {
        u_long mode = 1;
        if (::ioctlsocket(c, FIONBIO, &mode) != 0) {
            record_error();
            ::closesocket(c);
            return 0;
        }
    }
    record_success();
    return static_cast<uint64_t>(c);
}

int sock_connect(uint64_t s, uint32_t ip_be, uint16_t port_be) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip_be;
    addr.sin_port = port_be;
    if (::connect(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return record_error();
    }
    return record_success();
}

namespace {
// 本项目不使用 MSG_NOSIGNAL（Windows 无 SIGPIPE），其余标志透传可支持的位
int translate_msg_flags(int flags) {
    int out = 0;
    // winsock 没有 MSG_DONTWAIT：本项目所有 socket 均为非阻塞，语义已等价
    if (flags & 0x0002) { out |= MSG_PEEK; }       // MSG_PEEK
    return out;
}
}  // namespace

int sock_send(uint64_t s, const void* buf, int len, int flags) {
    int rc = ::send(static_cast<SOCKET>(s), static_cast<const char*>(buf), len,
                    translate_msg_flags(flags));
    if (rc == SOCKET_ERROR) {
        record_error();
        return -1;
    }
    record_success();
    return rc;
}

int sock_recv(uint64_t s, void* buf, int len, int flags) {
    int rc = ::recv(static_cast<SOCKET>(s), static_cast<char*>(buf), len,
                    translate_msg_flags(flags));
    if (rc == SOCKET_ERROR) {
        record_error();
        return -1;
    }
    record_success();
    return rc;
}

int sock_send_bufs(uint64_t s, const void* const* bufs, const int* lens, int count) {
    // 栈上足够（本项目 writev 最多 16 段）；更多段则截断并明确报错
    constexpr int kMaxBufs = 64;
    if (count <= 0 || count > kMaxBufs) {
        g_last_error = EINVAL;
        return -1;
    }
    WSABUF wsabufs[kMaxBufs];
    for (int i = 0; i < count; ++i) {
        wsabufs[i].buf = const_cast<char*>(static_cast<const char*>(bufs[i]));
        wsabufs[i].len = static_cast<ULONG>(lens[i]);
    }
    DWORD sent = 0;
    if (::WSASend(static_cast<SOCKET>(s), wsabufs, static_cast<DWORD>(count), &sent, 0,
                  nullptr, nullptr) == SOCKET_ERROR) {
        record_error();
        return -1;
    }
    record_success();
    return static_cast<int>(sent);
}

int sock_recv_bufs(uint64_t s, void* const* bufs, const int* lens, int count) {
    constexpr int kMaxBufs = 64;
    if (count <= 0 || count > kMaxBufs) {
        g_last_error = EINVAL;
        return -1;
    }
    WSABUF wsabufs[kMaxBufs];
    for (int i = 0; i < count; ++i) {
        wsabufs[i].buf = static_cast<char*>(bufs[i]);
        wsabufs[i].len = static_cast<ULONG>(lens[i]);
    }
    DWORD received = 0;
    DWORD flags = 0;
    if (::WSARecv(static_cast<SOCKET>(s), wsabufs, static_cast<DWORD>(count), &received,
                  &flags, nullptr, nullptr) == SOCKET_ERROR) {
        record_error();
        return -1;
    }
    record_success();
    return static_cast<int>(received);
}

int sock_setopt_int(uint64_t s, int opt, int value) {
    SOCKET sock = static_cast<SOCKET>(s);
    switch (opt) {
        case kOurTcpNoDelay: {
            // 通过 IPPROTO_TCP 设置，避免把 level 渗到接口层
            if (::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                             reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
                return record_error();
            }
            return record_success();
        }
        case kOurSoKeepAlive: {
            if (::setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,
                             reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
                return record_error();
            }
            return record_success();
        }
        case kOurSoReuseAddr:
            // Windows 的 SO_REUSEADDR 语义不同（允许他人抢占端口），
            // 这里显式忽略并返回成功，避免误用；端口复用由系统 TIME_WAIT 策略处理。
            return record_success();
        case kOurSoReusePort:
        case kOurTcpCork:
            // 不受支持：显式失败，让上层"不支持则降级/跳过"的逻辑生效
            g_last_error = EOPNOTSUPP;
            return -1;
        default:
            g_last_error = EOPNOTSUPP;
            return -1;
    }
}

int sock_setopt_timeo(uint64_t s, int opt, int seconds) {
    DWORD ms = seconds <= 0 ? 0 : static_cast<DWORD>(seconds) * 1000u;
    int level_opt = (opt == kOurSoRcvTimeo) ? SO_RCVTIMEO : SO_SNDTIMEO;
    if (::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, level_opt,
                     reinterpret_cast<const char*>(&ms), sizeof(ms)) != 0) {
        return record_error();
    }
    return record_success();
}

int sock_getopt_error(uint64_t s, int* out_value) {
    SOCKET sock = static_cast<SOCKET>(s);
    int err = 0;
    int len = static_cast<int>(sizeof(err));
    if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0) {
        return record_error();
    }
    if (out_value != nullptr) {
        *out_value = map_wsa_error(err);
    }
    return record_success();
}

int sock_local_addr(uint64_t s, uint32_t* ip_be, uint16_t* port_be) {
    sockaddr_in addr{};
    int len = static_cast<int>(sizeof(addr));
    if (::getsockname(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return record_error();
    }
    if (ip_be) { *ip_be = addr.sin_addr.s_addr; }
    if (port_be) { *port_be = addr.sin_port; }
    return record_success();
}

int sock_peer_addr(uint64_t s, uint32_t* ip_be, uint16_t* port_be) {
    sockaddr_in addr{};
    int len = static_cast<int>(sizeof(addr));
    if (::getpeername(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return record_error();
    }
    if (ip_be) { *ip_be = addr.sin_addr.s_addr; }
    if (port_be) { *port_be = addr.sin_port; }
    return record_success();
}

int sock_shutdown(uint64_t s, int how) {
    int wsa_how = SD_SEND;
    if (how == 0) { wsa_how = SD_RECEIVE; }
    else if (how == 2) { wsa_how = SD_BOTH; }
    if (::shutdown(static_cast<SOCKET>(s), wsa_how) != 0) {
        return record_error();
    }
    return record_success();
}

int sock_close(uint64_t s) {
    if (::closesocket(static_cast<SOCKET>(s)) != 0) {
        return record_error();
    }
    return record_success();
}

int sock_set_nonblocking(uint64_t s, bool on) {
    u_long mode = on ? 1u : 0u;
    if (::ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &mode) != 0) {
        return record_error();
    }
    return record_success();
}

int sock_poll(const uint64_t* handles, const short* events, short* revents, int count,
              int timeout_ms) {
    constexpr int kMaxFds = 1024;
    if (count < 0 || count > kMaxFds) {
        g_last_error = EINVAL;
        return -1;
    }
    WSAPOLLFD fds[kMaxFds];
    for (int i = 0; i < count; ++i) {
        fds[i].fd = static_cast<SOCKET>(handles[i]);
        fds[i].events = to_wsa_events(events[i]);
        fds[i].revents = 0;
    }
    int rc = ::WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
    if (rc == SOCKET_ERROR) {
        return record_error();
    }
    for (int i = 0; i < count; ++i) {
        revents[i] = from_wsa_events(fds[i].revents);
    }
    record_success();
    return rc;
}

uint64_t udp_self_pair(bool nonblocking) {
    ensure_init();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) {
        record_error();
        return 0;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        record_error();
        ::closesocket(s);
        return 0;
    }
    int len = static_cast<int>(sizeof(addr));
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        record_error();
        ::closesocket(s);
        return 0;
    }
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        record_error();
        ::closesocket(s);
        return 0;
    }
    if (nonblocking) {
        u_long mode = 1;
        if (::ioctlsocket(s, FIONBIO, &mode) != 0) {
            record_error();
            ::closesocket(s);
            return 0;
        }
    }
    record_success();
    return static_cast<uint64_t>(s);
}

std::size_t process_working_set_kb() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        return static_cast<std::size_t>(counters.WorkingSetSize / 1024);
    }
    return 0;
}

}  // namespace win32
}  // namespace tws
