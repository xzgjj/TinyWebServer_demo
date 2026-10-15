// =============================================================================
// Win32 兼容层：POSIX ABI 实现
//
// 架构：本 TU 只包含"自有 POSIX 头" + 中性后端接口（win32_backend.h /
//       win32_filemap.h），不引入 winsock/windows 类型 —— 因此不会有
//       "同一 TU 里两套 sockaddr_in" 的冲突。
//
// fd 模型：Windows 上 socket 不是 CRT 文件描述符。这里用【句柄表】把原生
//       SOCKET/epoll/eventfd 统一映射成小整数 fd（基址 100000，与 CRT fd 区间
//       天然分离），从而让既有代码里的 `int fd` 继续成立。
//       注意：这是"兼容层内部契约"，不是 Linux 语义的完全等价物 ——
//       例如 ::close(fd)（CRT）对托管 fd 无效，服务端代码统一走 tws::close_fd()。
// =============================================================================
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <poll.h>
#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "platform/win32_backend.h"
#include "platform/win32_filemap.h"
#include "platform/io_compat.h"

namespace {

enum class Kind : std::uint8_t { kFree = 0, kSocket, kEpoll, kEventFd };

struct Entry {
    Kind     kind = Kind::kFree;
    uint64_t native = 0;
    int      flags = 0;   ///< 兼容层维护的 fcntl 标志（当前只用 O_NONBLOCK）
};

struct EpollItem {
    int      fd = -1;
    uint32_t events = 0;
};

struct EpollState {
    std::vector<EpollItem> items;
};

constexpr int kHandleBase = 100000;

std::mutex g_mu;
std::vector<Entry> g_entries;
std::vector<int> g_free_slots;
std::unordered_map<int, EpollState> g_epolls;

int alloc_slot(Kind kind, std::uint64_t native, int flags = 0) {
    std::lock_guard<std::mutex> lock(g_mu);
    int index;
    if (!g_free_slots.empty()) {
        index = g_free_slots.back();
        g_free_slots.pop_back();
        g_entries[static_cast<std::size_t>(index)] = Entry{kind, native, flags};
    } else {
        index = static_cast<int>(g_entries.size());
        g_entries.push_back(Entry{kind, native, flags});
    }
    return kHandleBase + index;
}

/// 更新托管 fd 的 fcntl 标志位（F_SETFL 语义）
bool update_flags(int fd, int flags) {
    if (fd < kHandleBase) {
        return false;
    }
    const int index = fd - kHandleBase;
    std::lock_guard<std::mutex> lock(g_mu);
    if (index < 0 || index >= static_cast<int>(g_entries.size())) {
        return false;
    }
    Entry& e = g_entries[static_cast<std::size_t>(index)];
    if (e.kind == Kind::kFree) {
        return false;
    }
    e.flags = flags;
    return true;
}

bool lookup(int fd, Entry* out) {
    if (fd < kHandleBase) {
        return false;
    }
    const int index = fd - kHandleBase;
    std::lock_guard<std::mutex> lock(g_mu);
    if (index < 0 || index >= static_cast<int>(g_entries.size())) {
        return false;
    }
    const Entry& e = g_entries[static_cast<std::size_t>(index)];
    if (e.kind == Kind::kFree) {
        return false;
    }
    if (out != nullptr) {
        *out = e;
    }
    return true;
}

bool lookup_kind(int fd, Kind want, std::uint64_t* native) {
    Entry e;
    if (!lookup(fd, &e) || e.kind != want) {
        return false;
    }
    if (native != nullptr) {
        *native = e.native;
    }
    return true;
}

void release_slot(int fd) {
    if (fd < kHandleBase) {
        return;
    }
    const int index = fd - kHandleBase;
    std::lock_guard<std::mutex> lock(g_mu);
    if (index < 0 || index >= static_cast<int>(g_entries.size())) {
        return;
    }
    g_entries[static_cast<std::size_t>(index)] = Entry{};
    g_free_slots.push_back(index);
    g_epolls.erase(index);
}

int fail_with(int err) {
    errno = err;
    return -1;
}

}  // namespace

// -----------------------------------------------------------------------------
// 套接字
// -----------------------------------------------------------------------------
extern "C" int tws_socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) {
        return fail_with(EAFNOSUPPORT);
    }
    const bool stream = (type & 0x0F) == SOCK_STREAM;
    const bool nonblocking = (type & SOCK_NONBLOCK) != 0;
    const std::uint64_t native = tws::win32::sock_create(stream, nonblocking);
    if (native == 0) {
        return fail_with(tws::win32::last_error());
    }
    return alloc_slot(Kind::kSocket, native, nonblocking ? O_NONBLOCK : 0);
}

extern "C" int tws_bind(int fd, const struct sockaddr* addr, socklen_t len) {
    std::uint64_t native = 0;
    if (!lookup_kind(fd, Kind::kSocket, &native)) {
        return fail_with(EBADF);
    }
    if (addr == nullptr || len < static_cast<socklen_t>(sizeof(struct sockaddr_in))) {
        return fail_with(EINVAL);
    }
    const auto* in = reinterpret_cast<const struct sockaddr_in*>(addr);
    const int rc = tws::win32::sock_bind(native, in->sin_addr.s_addr, in->sin_port);
    return rc == 0 ? 0 : fail_with(rc);
}

extern "C" int tws_listen(int fd, int backlog) {
    std::uint64_t native = 0;
    if (!lookup_kind(fd, Kind::kSocket, &native)) {
        return fail_with(EBADF);
    }
    const int rc = tws::win32::sock_listen(native, backlog);
    return rc == 0 ? 0 : fail_with(rc);
}

namespace {

int accept_common(int fd, struct sockaddr* addr, socklen_t* len, int flags) {
    std::uint64_t native = 0;
    if (!lookup_kind(fd, Kind::kSocket, &native)) {
        return fail_with(EBADF);
    }
    const bool nonblocking = (flags & SOCK_NONBLOCK) != 0;
    const std::uint64_t client = tws::win32::sock_accept(native, nonblocking);
    if (client == 0) {
        return fail_with(tws::win32::last_error());
    }
    if (addr != nullptr && len != nullptr && *len >= static_cast<socklen_t>(sizeof(struct sockaddr_in))) {
        std::uint32_t ip = 0;
        std::uint16_t port = 0;
        if (tws::win32::sock_peer_addr(client, &ip, &port) == 0) {
            auto* in = reinterpret_cast<struct sockaddr_in*>(addr);
            std::memset(in, 0, sizeof(*in));
            in->sin_family = AF_INET;
            in->sin_addr.s_addr = ip;
            in->sin_port = port;
            *len = sizeof(struct sockaddr_in);
        }
    }
    return alloc_slot(Kind::kSocket, client, nonblocking ? O_NONBLOCK : 0);
}

}  // namespace

extern "C" int tws_accept(int fd, struct sockaddr* addr, socklen_t* len) {
    return accept_common(fd, addr, len, 0);
}

extern "C" int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags) {
    return accept_common(fd, addr, len, flags);
}

extern "C" int tws_connect(int fd, const struct sockaddr* addr, socklen_t len) {
    std::uint64_t native = 0;
    if (!lookup_kind(fd, Kind::kSocket, &native)) {
        return fail_with(EBADF);
    }
    if (addr == nullptr || len < static_cast<socklen_t>(sizeof(struct sockaddr_in))) {
        return fail_with(EINVAL);
    }
    const auto* in = reinterpret_cast<const struct sockaddr_in*>(addr);
    const int rc = tws::win32::sock_connect(native, in->sin_addr.s_addr, in->sin_port);
    return rc == 0 ? 0 : fail_with(rc);
}

namespace {

ssize_t send_common(int fd, const void* buf, size_t len, int flags) {
    std::uint64_t native = 0;
    if (!lookup(fd, nullptr)) {
        return fail_with(EBADF);
    }
    if (!lookup_kind(fd, Kind::kSocket, &native)) {
        // eventfd 也走 send 语义（自连 UDP）
        if (!lookup_kind(fd, Kind::kEventFd, &native)) {
            return fail_with(ENOTSOCK);
        }
    }
    const int rc = tws::win32::sock_send(native, buf, static_cast<int>(len), flags);
    return rc < 0 ? fail_with(tws::win32::last_error()) : static_cast<ssize_t>(rc);
}

ssize_t recv_common(int fd, void* buf, size_t len, int flags) {
    std::uint64_t native = 0;
    if (!lookup(fd, nullptr)) {
        return fail_with(EBADF);
    }
    if (!lookup_kind(fd, Kind::kSocket, &native) && !lookup_kind(fd, Kind::kEventFd, &native)) {
        return fail_with(ENOTSOCK);
    }
    const int rc = tws::win32::sock_recv(native, buf, static_cast<int>(len), flags);
    return rc < 0 ? fail_with(tws::win32::last_error()) : static_cast<ssize_t>(rc);
}

}  // namespace

extern "C" ssize_t tws_send(int fd, const void* buf, size_t len, int flags) {
    return send_common(fd, buf, len, flags);
}

extern "C" ssize_t tws_recv(int fd, void* buf, size_t len, int flags) {
    return recv_common(fd, buf, len, flags);
}

extern "C" ssize_t tws_sendto(int fd, const void* buf, size_t len, int flags,
                          const struct sockaddr* addr, socklen_t addrlen) {
    (void)addr;
    (void)addrlen;
    return send_common(fd, buf, len, flags);
}

extern "C" int tws_setsockopt(int fd, int level, int optname, const void* optval, socklen_t optlen) {
    (void)level;
    std::uint64_t native = 0;
    if (!lookup_kind(fd, Kind::kSocket, &native)) {
        return fail_with(EBADF);
    }
    if (optname == SO_SNDTIMEO || optname == SO_RCVTIMEO) {
        if (optval == nullptr || optlen < static_cast<socklen_t>(sizeof(struct timeval))) {
            return fail_with(EINVAL);
        }
        const auto* tv = reinterpret_cast<const struct timeval*>(optval);
        const int seconds = (tv->tv_sec == 0 && tv->tv_usec > 0) ? 1 : static_cast<int>(tv->tv_sec);
        const int rc = tws::win32::sock_setopt_timeo(native, optname, seconds);
        return rc == 0 ? 0 : fail_with(rc);
    }
    int value = 0;
    if (optval != nullptr && optlen >= static_cast<socklen_t>(sizeof(int))) {
        value = *reinterpret_cast<const int*>(optval);
    }
    const int rc = tws::win32::sock_setopt_int(native, optname, value);
    return rc == 0 ? 0 : fail_with(rc);
}

extern "C" int tws_getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen) {
    (void)level;
    std::uint64_t native = 0;
    if (!lookup_kind(fd, Kind::kSocket, &native)) {
        return fail_with(EBADF);
    }
    if (optname == SO_ERROR && optval != nullptr && optlen != nullptr &&
        *optlen >= static_cast<socklen_t>(sizeof(int))) {
        int err = 0;
        const int rc = tws::win32::sock_getopt_error(native, &err);
        if (rc != 0) {
            return fail_with(rc);
        }
        *reinterpret_cast<int*>(optval) = err;
        return 0;
    }
    return fail_with(ENOPROTOOPT);
}

extern "C" int tws_getsockname(int fd, struct sockaddr* addr, socklen_t* len) {
    std::uint64_t native = 0;
    if (!lookup_kind(fd, Kind::kSocket, &native)) {
        return fail_with(EBADF);
    }
    if (addr == nullptr || len == nullptr) {
        return fail_with(EINVAL);
    }
    std::uint32_t ip = 0;
    std::uint16_t port = 0;
    const int rc = tws::win32::sock_local_addr(native, &ip, &port);
    if (rc != 0) {
        return fail_with(rc);
    }
    auto* in = reinterpret_cast<struct sockaddr_in*>(addr);
    std::memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = ip;
    in->sin_port = port;
    *len = sizeof(struct sockaddr_in);
    return 0;
}

extern "C" int tws_getpeername(int fd, struct sockaddr* addr, socklen_t* len) {
    std::uint64_t native = 0;
    if (!lookup_kind(fd, Kind::kSocket, &native)) {
        return fail_with(EBADF);
    }
    if (addr == nullptr || len == nullptr) {
        return fail_with(EINVAL);
    }
    std::uint32_t ip = 0;
    std::uint16_t port = 0;
    const int rc = tws::win32::sock_peer_addr(native, &ip, &port);
    if (rc != 0) {
        return fail_with(rc);
    }
    auto* in = reinterpret_cast<struct sockaddr_in*>(addr);
    std::memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = ip;
    in->sin_port = port;
    *len = sizeof(struct sockaddr_in);
    return 0;
}

extern "C" int tws_shutdown(int fd, int how) {
    std::uint64_t native = 0;
    if (!lookup(fd, nullptr)) {
        return fail_with(EBADF);
    }
    if (!lookup_kind(fd, Kind::kSocket, &native) && !lookup_kind(fd, Kind::kEventFd, &native)) {
        return fail_with(ENOTSOCK);
    }
    const int rc = tws::win32::sock_shutdown(native, how);
    return rc == 0 ? 0 : fail_with(rc);
}

// -----------------------------------------------------------------------------
// 分散/聚集 I/O
// -----------------------------------------------------------------------------
extern "C" ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    std::uint64_t native = 0;
    if (!lookup(fd, nullptr)) {
        return fail_with(EBADF);
    }
    if (!lookup_kind(fd, Kind::kSocket, &native) && !lookup_kind(fd, Kind::kEventFd, &native)) {
        return fail_with(ENOTSOCK);
    }
    if (iov == nullptr || iovcnt <= 0 || iovcnt > 64) {
        return fail_with(EINVAL);
    }
    const void* bufs[64];
    int lens[64];
    for (int i = 0; i < iovcnt; ++i) {
        bufs[i] = iov[i].iov_base;
        lens[i] = static_cast<int>(iov[i].iov_len);
    }
    const int rc = tws::win32::sock_send_bufs(native, bufs, lens, iovcnt);
    return rc < 0 ? fail_with(tws::win32::last_error()) : static_cast<ssize_t>(rc);
}

extern "C" ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    std::uint64_t native = 0;
    if (!lookup_kind(fd, Kind::kSocket, &native)) {
        return fail_with(EBADF);
    }
    if (iov == nullptr || iovcnt <= 0 || iovcnt > 64) {
        return fail_with(EINVAL);
    }
    void* bufs[64];
    int lens[64];
    for (int i = 0; i < iovcnt; ++i) {
        bufs[i] = iov[i].iov_base;
        lens[i] = static_cast<int>(iov[i].iov_len);
    }
    const int rc = tws::win32::sock_recv_bufs(native, bufs, lens, iovcnt);
    return rc < 0 ? fail_with(tws::win32::last_error()) : static_cast<ssize_t>(rc);
}

// -----------------------------------------------------------------------------
// epoll（基于 WSAPoll 的水平触发实现；EPOLLET 被接受但不改变行为）
// -----------------------------------------------------------------------------
extern "C" int epoll_create1(int flags) {
    (void)flags;
    const int fd = alloc_slot(Kind::kEpoll, 0);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_epolls[fd - kHandleBase] = EpollState{};
    }
    return fd;
}

extern "C" int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
    if (!lookup_kind(epfd, Kind::kEpoll, nullptr)) {
        return fail_with(EBADF);
    }
    const int index = epfd - kHandleBase;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_epolls.find(index);
    if (it == g_epolls.end()) {
        return fail_with(EBADF);
    }
    EpollState& state = it->second;

    auto find_item = [&state](int target) {
        for (auto item = state.items.begin(); item != state.items.end(); ++item) {
            if (item->fd == target) {
                return item;
            }
        }
        return state.items.end();
    };

    switch (op) {
        case EPOLL_CTL_ADD: {
            if (event == nullptr) {
                return fail_with(EINVAL);
            }
            if (find_item(fd) != state.items.end()) {
                return fail_with(EEXIST);
            }
            state.items.push_back(EpollItem{fd, event->events});
            return 0;
        }
        case EPOLL_CTL_MOD: {
            if (event == nullptr) {
                return fail_with(EINVAL);
            }
            auto item = find_item(fd);
            if (item == state.items.end()) {
                return fail_with(ENOENT);
            }
            item->events = event->events;
            return 0;
        }
        case EPOLL_CTL_DEL: {
            auto item = find_item(fd);
            if (item == state.items.end()) {
                return fail_with(ENOENT);
            }
            state.items.erase(item);
            return 0;
        }
        default:
            return fail_with(EINVAL);
    }
}

extern "C" int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    if (!lookup_kind(epfd, Kind::kEpoll, nullptr) || events == nullptr || maxevents <= 0) {
        return fail_with(EINVAL);
    }
    std::vector<EpollItem> items;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_epolls.find(epfd - kHandleBase);
        if (it == g_epolls.end()) {
            return fail_with(EBADF);
        }
        items = it->second.items;
    }

    std::vector<std::uint64_t> handles;
    std::vector<short> poll_events;
    std::vector<int> fds;
    handles.reserve(items.size());
    poll_events.reserve(items.size());
    fds.reserve(items.size());

    for (const EpollItem& item : items) {
        std::uint64_t native = 0;
        if (!lookup(item.fd, nullptr)) {
            continue;  // 已被关闭的 fd：跳过（等价于内核自动摘除）
        }
        if (!lookup_kind(item.fd, Kind::kSocket, &native) &&
            !lookup_kind(item.fd, Kind::kEventFd, &native)) {
            continue;
        }
        short mask = 0;
        if ((item.events & (EPOLLIN | EPOLLRDHUP)) != 0u) { mask |= POLLIN; }
        if ((item.events & EPOLLOUT) != 0u) { mask |= POLLOUT; }
        if ((item.events & EPOLLPRI) != 0u) { mask |= POLLPRI; }
        handles.push_back(native);
        poll_events.push_back(mask);
        fds.push_back(item.fd);
    }

    if (handles.empty()) {
        // 没有可等待的 fd：按 POSIX 语义"睡满超时"而不是忙等
        if (timeout > 0) {
            return 0;
        }
        return 0;
    }

    std::vector<short> revents(handles.size(), 0);
    const int rc = tws::win32::sock_poll(handles.data(), poll_events.data(), revents.data(),
                                         static_cast<int>(handles.size()), timeout);
    if (rc < 0) {
        return fail_with(tws::win32::last_error());
    }
    if (rc == 0) {
        return 0;
    }

    int out = 0;
    std::vector<int> oneshot_fds;
    for (std::size_t i = 0; i < handles.size() && out < maxevents; ++i) {
        short ready = revents[i];
        if (ready == 0) {
            continue;
        }
        std::uint32_t ev = 0;
        if ((ready & POLLIN) != 0) { ev |= EPOLLIN; }
        if ((ready & POLLOUT) != 0) { ev |= EPOLLOUT; }
        if ((ready & POLLPRI) != 0) { ev |= EPOLLPRI; }
        if ((ready & POLLERR) != 0) { ev |= EPOLLERR; }
        if ((ready & POLLHUP) != 0) { ev |= EPOLLHUP; }
        events[out].events = ev;
        events[out].data.fd = fds[i];
        ++out;

        // 一次性语义：触发后摘除，等待上层 MOD 重新武装
        for (const EpollItem& item : items) {
            if (item.fd == fds[i] && (item.events & EPOLLONESHOT) != 0u) {
                oneshot_fds.push_back(item.fd);
            }
        }
    }
    if (!oneshot_fds.empty()) {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_epolls.find(epfd - kHandleBase);
        if (it != g_epolls.end()) {
            auto& vec = it->second.items;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [&oneshot_fds](const EpollItem& item) {
                                         for (int fd : oneshot_fds) {
                                             if (fd == item.fd) { return true; }
                                         }
                                         return false;
                                     }),
                      vec.end());
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// eventfd（自连 UDP 实现，可被 epoll_wait/poll 等待）
// -----------------------------------------------------------------------------
extern "C" int eventfd(unsigned int initval, int flags) {
    const bool nonblocking = (flags & EFD_NONBLOCK) != 0;
    const std::uint64_t native = tws::win32::udp_self_pair(nonblocking);
    if (native == 0) {
        return fail_with(tws::win32::last_error());
    }
    const int fd = alloc_slot(Kind::kEventFd, native, nonblocking ? O_NONBLOCK : 0);
    if (initval != 0) {
        const std::uint64_t value = initval;
        (void)tws_io_write(fd, &value, sizeof(value));
    }
    return fd;
}

// -----------------------------------------------------------------------------
// poll
// -----------------------------------------------------------------------------
extern "C" int poll(struct pollfd* fds, unsigned long nfds, int timeout) {
    if (fds == nullptr) {
        return fail_with(EINVAL);
    }
    std::vector<std::uint64_t> handles;
    std::vector<short> events;
    std::vector<int> indexes;
    for (unsigned long i = 0; i < nfds; ++i) {
        fds[i].revents = 0;
        std::uint64_t native = 0;
        if (!lookup(fds[i].fd, nullptr) ||
            (!lookup_kind(fds[i].fd, Kind::kSocket, &native) &&
             !lookup_kind(fds[i].fd, Kind::kEventFd, &native))) {
            fds[i].revents = POLLNVAL;
            continue;
        }
        handles.push_back(native);
        events.push_back(fds[i].events);
        indexes.push_back(static_cast<int>(i));
    }
    if (handles.empty()) {
        return 0;
    }
    std::vector<short> revents(handles.size(), 0);
    const int rc = tws::win32::sock_poll(handles.data(), events.data(), revents.data(),
                                         static_cast<int>(handles.size()), timeout);
    if (rc < 0) {
        return fail_with(tws::win32::last_error());
    }
    int ready = 0;
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        if (revents[i] != 0) {
            fds[static_cast<std::size_t>(indexes[i])].revents = revents[i];
            ++ready;
        }
    }
    return ready;
}

// -----------------------------------------------------------------------------
// mmap / munmap（只读文件映射）
// -----------------------------------------------------------------------------
extern "C" void* mmap(void* addr, size_t length, int prot, int flags, int fd, long offset) {
    (void)addr;
    (void)prot;
    (void)flags;
    (void)offset;
    if (fd < 0) {
        errno = ENOTSUP;   // 匿名映射未实现：显式失败，绝不返回假指针
        return MAP_FAILED;
    }
    void* view = tws::win32::map_file_readonly(fd, length);
    if (view == nullptr) {
        errno = EIO;
        return MAP_FAILED;
    }
    return view;
}

extern "C" int munmap(void* addr, size_t length) {
    if (tws::win32::unmap_file(addr, length) != 0) {
        return fail_with(EINVAL);
    }
    return 0;
}

// -----------------------------------------------------------------------------
// fcntl（只实现 F_GETFL / F_SETFL 的 O_NONBLOCK 语义）
// -----------------------------------------------------------------------------
extern "C" int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    const int arg = va_arg(ap, int);
    va_end(ap);

    std::uint64_t native = 0;
    const bool managed = lookup_kind(fd, Kind::kSocket, &native) ||
                         lookup_kind(fd, Kind::kEventFd, &native);
    switch (cmd) {
        case F_GETFL: {
            // 返回兼容层维护的标志（仅 O_NONBLOCK），而不是硬编码 0：
            // test_basic 等用例会断言 (flags & O_NONBLOCK)。
            if (!managed) {
                return fail_with(EBADF);
            }
            Entry entry;
            if (!lookup(fd, &entry)) {
                return fail_with(EBADF);
            }
            return entry.flags;
        }
        case F_SETFL: {
            if (!managed) {
                return fail_with(EBADF);
            }
            const bool on = (arg & O_NONBLOCK) != 0;
            const int rc = tws::win32::sock_set_nonblocking(native, on);
            if (rc != 0) {
                return fail_with(rc);
            }
            Entry entry;
            if (lookup(fd, &entry)) {
                const int flags = on ? (entry.flags | O_NONBLOCK) : (entry.flags & ~O_NONBLOCK);
                update_flags(fd, flags);
            }
            return 0;
        }
        default:
            return fail_with(EINVAL);
    }
}

// -----------------------------------------------------------------------------
// getrusage（只填 ru_maxrss；其余为 0，明确不伪造）
// -----------------------------------------------------------------------------
extern "C" int getrusage(int who, struct rusage* usage) {
    (void)who;
    if (usage == nullptr) {
        return fail_with(EINVAL);
    }
    std::memset(usage, 0, sizeof(*usage));
    usage->ru_maxrss = static_cast<long>(tws::win32::process_working_set_kb());
    return 0;
}

// -----------------------------------------------------------------------------
// inet_pton / inet_ntop（IPv4）
// -----------------------------------------------------------------------------
extern "C" int tws_inet_pton(int af, const char* src, void* dst) {
    if (af != AF_INET || src == nullptr || dst == nullptr) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    unsigned int a = 0;
    unsigned int b = 0;
    unsigned int c = 0;
    unsigned int d = 0;
    char tail = '\0';
    if (std::sscanf(src, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return 0;
    }
    if (a > 255u || b > 255u || c > 255u || d > 255u) {
        return 0;
    }
    const std::uint32_t host = (a << 24) | (b << 16) | (c << 8) | d;
    // s_addr 使用网络字节序
    reinterpret_cast<std::uint32_t*>(dst)[0] =
        ((host & 0x000000ffu) << 24) | ((host & 0x0000ff00u) << 8) |
        ((host & 0x00ff0000u) >> 8)  | ((host & 0xff000000u) >> 24);
    return 1;
}

extern "C" const char* tws_inet_ntop(int af, const void* src, char* dst, socklen_t size) {
    if (af != AF_INET || src == nullptr || dst == nullptr) {
        errno = EAFNOSUPPORT;
        return nullptr;
    }
    const std::uint32_t be = reinterpret_cast<const std::uint32_t*>(src)[0];
    const std::uint32_t host = ((be & 0x000000ffu) << 24) | ((be & 0x0000ff00u) << 8) |
                               ((be & 0x00ff0000u) >> 8)  | ((be & 0xff000000u) >> 24);
    char buf[16];
    const int n = std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                                (host >> 24) & 0xffu, (host >> 16) & 0xffu,
                                (host >> 8) & 0xffu, host & 0xffu);
    if (n <= 0 || static_cast<socklen_t>(n) >= size) {
        errno = ENOSPC;
        return nullptr;
    }
    std::memcpy(dst, buf, static_cast<std::size_t>(n) + 1u);
    return dst;
}

// -----------------------------------------------------------------------------
// tws::io_compat 的实现（服务端 fd I/O 统一入口）
// -----------------------------------------------------------------------------
extern "C" int tws_io_is_managed(int fd) {
    return lookup(fd, nullptr) ? 1 : 0;
}

extern "C" int tws_io_close(int fd) {
    Entry entry;
    if (!lookup(fd, &entry)) {
        return _close(fd);   // 非托管：CRT 文件描述符
    }
    int rc = 0;
    if (entry.kind == Kind::kSocket || entry.kind == Kind::kEventFd) {
        rc = tws::win32::sock_close(entry.native);
    }
    release_slot(fd);
    if (rc != 0) {
        return fail_with(rc);
    }
    return 0;
}

extern "C" ssize_t tws_io_read(int fd, void* buf, size_t count) {
    Entry entry;
    if (!lookup(fd, &entry)) {
        return _read(fd, buf, static_cast<unsigned int>(count));
    }
    const int rc = tws::win32::sock_recv(entry.native, buf, static_cast<int>(count), 0);
    return rc < 0 ? fail_with(tws::win32::last_error()) : static_cast<ssize_t>(rc);
}

extern "C" ssize_t tws_io_write(int fd, const void* buf, size_t count) {
    Entry entry;
    if (!lookup(fd, &entry)) {
        return _write(fd, buf, static_cast<unsigned int>(count));
    }
    const int rc = tws::win32::sock_send(entry.native, buf, static_cast<int>(count), 0);
    return rc < 0 ? fail_with(tws::win32::last_error()) : static_cast<ssize_t>(rc);
}

extern "C" int tws_io_shutdown(int fd, int how) {
    Entry entry;
    if (!lookup(fd, &entry)) {
        return fail_with(EBADF);
    }
    const int rc = tws::win32::sock_shutdown(entry.native, how);
    return rc == 0 ? 0 : fail_with(rc);
}

extern "C" int tws_io_set_nonblocking(int fd, int on) {
    Entry entry;
    if (!lookup(fd, &entry)) {
        return fail_with(EBADF);
    }
    const int rc = tws::win32::sock_set_nonblocking(entry.native, on != 0);
    if (rc != 0) {
        return fail_with(rc);
    }
    const int flags = (on != 0) ? (entry.flags | O_NONBLOCK) : (entry.flags & ~O_NONBLOCK);
    update_flags(fd, flags);
    return 0;
}
