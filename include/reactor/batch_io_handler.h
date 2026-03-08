#pragma once

#include <sys/epoll.h>
#include <functional>
#include <vector>

/**
 * @brief BatchIOHandler class for efficient batch processing of epoll events
 *
 * This class optimizes I/O event processing by grouping events by type
 * and processing them in batches, reducing function call overhead and
 * improving cache locality.
 */
class BatchIOHandler {
public:
    /**
     * @brief Result statistics for batch processing
     */
    struct BatchResult {
        size_t total_processed = 0;      // Total events processed
        size_t read_processed = 0;       // Read events processed
        size_t write_processed = 0;      // Write events processed
        size_t error_processed = 0;      // Error events processed
    };

    /**
     * @brief Process a batch of epoll events
     *
     * Groups events by type (readable, writable, errors) and calls
     * appropriate callbacks in batches for better performance.
     *
     * @param events Vector of epoll events to process
     * @param read_callback Callback for readable events (fd as parameter)
     * @param write_callback Callback for writable events (fd as parameter)
     * @param error_callback Callback for error events (fd as parameter)
     * @return BatchResult with processing statistics
     */
    BatchResult ProcessBatch(const std::vector<epoll_event>& events,
                             const std::function<void(int)>& read_callback,
                             const std::function<void(int)>& write_callback,
                             const std::function<void(int)>& error_callback);

    /**
     * @brief Process a batch of readable file descriptors
     *
     * Optimized version for processing multiple readable fds at once.
     *
     * @param fds Vector of file descriptors to process as readable
     * @param callback Callback for each file descriptor
     */
    void BatchRead(const std::vector<int>& fds,
                   const std::function<void(int)>& callback);

    /**
     * @brief Process a batch of writable file descriptors
     *
     * Optimized version for processing multiple writable fds at once.
     *
     * @param fds Vector of file descriptors to process as writable
     * @param callback Callback for each file descriptor
     */
    void BatchWrite(const std::vector<int>& fds,
                    const std::function<void(int)>& callback);

    /**
     * @brief Process a batch of error file descriptors
     *
     * Optimized version for processing multiple error fds at once.
     *
     * @param fds Vector of file descriptors to process as errors
     * @param callback Callback for each file descriptor
     */
    void BatchError(const std::vector<int>& fds,
                    const std::function<void(int)>& callback);

private:
    /**
     * @brief Group events by type (readable, writable, errors)
     *
     * @param events Input events to group
     * @param readable Output vector for readable fds
     * @param writable Output vector for writable fds
     * @param errors Output vector for error fds
     */
    void GroupEvents(const std::vector<epoll_event>& events,
                     std::vector<int>& readable,
                     std::vector<int>& writable,
                     std::vector<int>& errors) const;

    /**
     * @brief Check if events contain readable condition
     *
     * @param events Epoll events mask
     * @return true if readable (EPOLLIN or EPOLLPRI)
     */
    static bool IsReadable(uint32_t events) {
        return (events & (EPOLLIN | EPOLLPRI)) != 0;
    }

    /**
     * @brief Check if events contain writable condition
     *
     * @param events Epoll events mask
     * @return true if writable (EPOLLOUT)
     */
    static bool IsWritable(uint32_t events) {
        return (events & EPOLLOUT) != 0;
    }

    /**
     * @brief Check if events contain error condition
     *
     * @param events Epoll events mask
     * @return true if error (EPOLLERR, EPOLLHUP, or EPOLLRDHUP)
     */
    static bool IsError(uint32_t events) {
        return (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
    }
};