//

#include "reactor/socket_utils.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

void SetNonBlocking(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        throw std::runtime_error("fcntl(F_GETFL) failed");
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        throw std::runtime_error("fcntl(F_SETFL) failed");
    }
}

int CreateListenSocket(unsigned short port, int backlog, bool tcp_nodelay, bool tcp_cork)
{
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        throw std::runtime_error("socket failed");
    }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 设置 TCP_NODELAY（禁用 Nagle 算法）
    if (tcp_nodelay) {
        opt = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    }

    // 设置 TCP_CORK（聚合小包）
    if (tcp_cork) {
        opt = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_CORK, &opt, sizeof(opt));
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        throw std::runtime_error("bind failed");
    }

    if (::listen(fd, backlog) < 0)
    {
        ::close(fd);
        throw std::runtime_error("listen failed");
    }

    return fd;
}
