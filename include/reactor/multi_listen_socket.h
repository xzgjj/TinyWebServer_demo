#pragma once

#include <string>
#include <vector>
#include <cstdint>

/**
 * @brief MultiListenSocket class for managing multiple listening sockets with SO_REUSEPORT
 *
 * This class creates multiple listening sockets bound to the same IP and port
 * with the SO_REUSEPORT option enabled, allowing kernel-level load balancing
 * across multiple threads/processes.
 */
class MultiListenSocket {
public:
    /**
     * @brief Constructor for MultiListenSocket
     *
     * @param ip IP address to bind to (e.g., "0.0.0.0" for all interfaces)
     * @param port Port number to bind to
     * @param num_sockets Number of listening sockets to create
     * @param backlog Maximum length of the queue of pending connections
     * @param tcp_nodelay Enable TCP_NODELAY option
     * @param tcp_cork Enable TCP_CORK option
     */
    explicit MultiListenSocket(const std::string& ip, uint16_t port,
                               size_t num_sockets, int backlog = 1024,
                               bool tcp_nodelay = true, bool tcp_cork = false);

    /**
     * @brief Destructor - closes all sockets
     */
    ~MultiListenSocket();

    // Non-copyable
    MultiListenSocket(const MultiListenSocket&) = delete;
    MultiListenSocket& operator=(const MultiListenSocket&) = delete;

    // Move operations
    MultiListenSocket(MultiListenSocket&& other) noexcept;
    MultiListenSocket& operator=(MultiListenSocket&& other) noexcept;

    /**
     * @brief Get socket file descriptor at specified index
     *
     * @param index Index of the socket (0-based)
     * @return Socket file descriptor, or -1 if index is invalid
     */
    int GetSocketFd(size_t index) const;

    /**
     * @brief Get all socket file descriptors
     *
     * @return Vector of all socket file descriptors
     */
    const std::vector<int>& GetAllSocketFds() const { return socket_fds_; }

    /**
     * @brief Check if all sockets were successfully created
     *
     * @return true if all sockets are valid, false otherwise
     */
    bool IsValid() const { return is_valid_; }

    /**
     * @brief Get error message if creation failed
     *
     * @return Error message string, empty if no error
     */
    const std::string& GetError() const { return error_message_; }

    /**
     * @brief Get the number of sockets
     */
    size_t GetNumSockets() const { return socket_fds_.size(); }

    /**
     * @brief Close all sockets and clear resources
     */
    void CloseAll();

private:
    /**
     * @brief Create and bind all sockets
     *
     * @return true if successful, false otherwise
     */
    bool CreateAndBindSockets();

    /**
     * @brief Create a single listening socket with SO_REUSEPORT option
     *
     * @param ip IP address to bind
     * @param port Port number to bind
     * @param backlog Maximum length of the queue of pending connections
     * @param tcp_nodelay Enable TCP_NODELAY option
     * @param tcp_cork Enable TCP_CORK option
     * @return Socket file descriptor, or -1 on error
     */
    int CreateListenSocket(const std::string& ip, uint16_t port,
                           int backlog, bool tcp_nodelay, bool tcp_cork);

    /**
     * @brief Set socket options (SO_REUSEADDR, SO_REUSEPORT, TCP_NODELAY, TCP_CORK)
     *
     * @param fd Socket file descriptor
     * @param tcp_nodelay Enable TCP_NODELAY option
     * @param tcp_cork Enable TCP_CORK option
     * @return true if successful, false otherwise
     */
    bool SetSocketOptions(int fd, bool tcp_nodelay, bool tcp_cork);

    /**
     * @brief Check if SO_REUSEPORT is supported by the system
     *
     * @return true if supported, false otherwise
     */
    static bool IsSOReusePortSupported();

    std::vector<int> socket_fds_;
    std::string error_message_;
    bool is_valid_;
    std::string ip_;
    uint16_t port_;
    int backlog_;
    bool tcp_nodelay_;
    bool tcp_cork_;
};