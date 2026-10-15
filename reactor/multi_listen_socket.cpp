#include "reactor/multi_listen_socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include "Logger.h"

namespace {

// Helper function to set socket options
bool SetSocketOption(int fd, int level, int optname, int value) {
    return ::setsockopt(fd, level, optname, &value, sizeof(value)) == 0;
}

// Helper function to create socket address
sockaddr_in CreateSocketAddress(const std::string& ip, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (ip.empty() || ip == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            throw std::runtime_error("Invalid IP address: " + ip);
        }
    }

    return addr;
}

} // anonymous namespace

MultiListenSocket::MultiListenSocket(const std::string& ip, uint16_t port,
                                     size_t num_sockets, int backlog,
                                     bool tcp_nodelay, bool tcp_cork)
    : is_valid_(false),
      ip_(ip),
      port_(port),
      backlog_(backlog),
      tcp_nodelay_(tcp_nodelay),
      tcp_cork_(tcp_cork) {

    if (num_sockets == 0) {
        error_message_ = "Number of sockets must be greater than 0";
        return;
    }

    // Check if SO_REUSEPORT is supported
    if (!IsSOReusePortSupported()) {
        error_message_ = "SO_REUSEPORT is not supported on this system (requires Linux 3.9+)";
        return;
    }

    // Reserve space for sockets
    socket_fds_.reserve(num_sockets);

    // Create and bind all sockets
    if (!CreateAndBindSockets()) {
        CloseAll();
        return;
    }

    is_valid_ = true;
    LOG_INFO("MultiListenSocket created with %zu sockets on %s:%u",
             socket_fds_.size(), ip.c_str(), port);
}

MultiListenSocket::~MultiListenSocket() {
    CloseAll();
}

MultiListenSocket::MultiListenSocket(MultiListenSocket&& other) noexcept
    : socket_fds_(std::move(other.socket_fds_)),
      error_message_(std::move(other.error_message_)),
      is_valid_(other.is_valid_),
      ip_(std::move(other.ip_)),
      port_(other.port_),
      backlog_(other.backlog_),
      tcp_nodelay_(other.tcp_nodelay_),
      tcp_cork_(other.tcp_cork_) {
    other.socket_fds_.clear();
    other.is_valid_ = false;
}

MultiListenSocket& MultiListenSocket::operator=(MultiListenSocket&& other) noexcept {
    if (this != &other) {
        CloseAll();

        socket_fds_ = std::move(other.socket_fds_);
        error_message_ = std::move(other.error_message_);
        is_valid_ = other.is_valid_;
        ip_ = std::move(other.ip_);
        port_ = other.port_;
        backlog_ = other.backlog_;
        tcp_nodelay_ = other.tcp_nodelay_;
        tcp_cork_ = other.tcp_cork_;

        other.socket_fds_.clear();
        other.is_valid_ = false;
    }
    return *this;
}

int MultiListenSocket::GetSocketFd(size_t index) const {
    if (index >= socket_fds_.size()) {
        return -1;
    }
    return socket_fds_[index];
}

void MultiListenSocket::CloseAll() {
    for (int fd : socket_fds_) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    socket_fds_.clear();
    is_valid_ = false;
}

bool MultiListenSocket::CreateAndBindSockets() {
    for (size_t i = 0; i < socket_fds_.capacity(); ++i) {
        int fd = CreateListenSocket(ip_, port_, backlog_, tcp_nodelay_, tcp_cork_);
        if (fd < 0) {
            error_message_ = "Failed to create socket " + std::to_string(i) +
                            ": " + std::strerror(errno);
            return false;
        }
        socket_fds_.push_back(fd);
    }
    return true;
}

int MultiListenSocket::CreateListenSocket(const std::string& ip, uint16_t port,
                                          int backlog, bool tcp_nodelay, bool tcp_cork) {
    // Create socket
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    // Set socket options
    if (!SetSocketOptions(fd, tcp_nodelay, tcp_cork)) {
        ::close(fd);
        return -1;
    }

    // Create address
    sockaddr_in addr = CreateSocketAddress(ip, port);

    // Bind
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    // Listen
    if (::listen(fd, backlog) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

bool MultiListenSocket::SetSocketOptions(int fd, bool tcp_nodelay, bool tcp_cork) {
    // Set SO_REUSEADDR (always enabled for safety)
    if (!SetSocketOption(fd, SOL_SOCKET, SO_REUSEADDR, 1)) {
        return false;
    }

    // Set SO_REUSEPORT (key feature for multi-socket listening)
    if (!SetSocketOption(fd, SOL_SOCKET, SO_REUSEPORT, 1)) {
        return false;
    }

    // Set TCP_NODELAY (disable Nagle algorithm)
    if (tcp_nodelay && !SetSocketOption(fd, IPPROTO_TCP, TCP_NODELAY, 1)) {
        return false;
    }

    // Set TCP_CORK (aggregate small packets)
    if (tcp_cork && !SetSocketOption(fd, IPPROTO_TCP, TCP_CORK, 1)) {
        return false;
    }

    return true;
}

bool MultiListenSocket::IsSOReusePortSupported() {
    // Create a test socket to check SO_REUSEPORT support
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    int optval = 1;
    bool supported = (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) == 0);

    ::close(fd);
    return supported;
}