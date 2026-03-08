#include "reactor/batch_io_handler.h"

#include <algorithm>
#include <cassert>

#include "Logger.h"

BatchIOHandler::BatchResult BatchIOHandler::ProcessBatch(
    const std::vector<epoll_event>& events,
    const std::function<void(int)>& read_callback,
    const std::function<void(int)>& write_callback,
    const std::function<void(int)>& error_callback) {

    BatchResult result;
    if (events.empty()) {
        return result;
    }

    // Group events by type for batch processing
    std::vector<int> readable, writable, errors;
    GroupEvents(events, readable, writable, errors);

    // Process readable events in batch
    if (!readable.empty() && read_callback) {
        result.read_processed = readable.size();
        BatchRead(readable, read_callback);
    }

    // Process writable events in batch
    if (!writable.empty() && write_callback) {
        result.write_processed = writable.size();
        BatchWrite(writable, write_callback);
    }

    // Process error events in batch
    if (!errors.empty() && error_callback) {
        result.error_processed = errors.size();
        BatchError(errors, error_callback);
    }

    result.total_processed = events.size();
    return result;
}

void BatchIOHandler::BatchRead(const std::vector<int>& fds,
                               const std::function<void(int)>& callback) {
    assert(callback);

    // Simple loop for now - can be optimized with prefetching or SIMD
    for (int fd : fds) {
        try {
            callback(fd);
        } catch (const std::exception& e) {
            LOG_ERROR("BatchIOHandler::BatchRead callback failed for fd=%d: %s",
                      fd, e.what());
        } catch (...) {
            LOG_ERROR("BatchIOHandler::BatchRead callback failed for fd=%d: unknown exception",
                      fd);
        }
    }

    LOG_DEBUG("BatchIOHandler::BatchRead processed %zu fds", fds.size());
}

void BatchIOHandler::BatchWrite(const std::vector<int>& fds,
                                const std::function<void(int)>& callback) {
    assert(callback);

    for (int fd : fds) {
        try {
            callback(fd);
        } catch (const std::exception& e) {
            LOG_ERROR("BatchIOHandler::BatchWrite callback failed for fd=%d: %s",
                      fd, e.what());
        } catch (...) {
            LOG_ERROR("BatchIOHandler::BatchWrite callback failed for fd=%d: unknown exception",
                      fd);
        }
    }

    LOG_DEBUG("BatchIOHandler::BatchWrite processed %zu fds", fds.size());
}

void BatchIOHandler::BatchError(const std::vector<int>& fds,
                                const std::function<void(int)>& callback) {
    assert(callback);

    for (int fd : fds) {
        try {
            callback(fd);
        } catch (const std::exception& e) {
            LOG_ERROR("BatchIOHandler::BatchError callback failed for fd=%d: %s",
                      fd, e.what());
        } catch (...) {
            LOG_ERROR("BatchIOHandler::BatchError callback failed for fd=%d: unknown exception",
                      fd);
        }
    }

    LOG_DEBUG("BatchIOHandler::BatchError processed %zu fds", fds.size());
}

void BatchIOHandler::GroupEvents(const std::vector<epoll_event>& events,
                                 std::vector<int>& readable,
                                 std::vector<int>& writable,
                                 std::vector<int>& errors) const {
    // Reserve space for efficiency
    readable.reserve(events.size());
    writable.reserve(events.size());
    errors.reserve(events.size());

    for (const auto& event : events) {
        int fd = event.data.fd;
        uint32_t revents = event.events;

        bool has_read = IsReadable(revents);
        bool has_write = IsWritable(revents);
        bool has_error = IsError(revents);

        // Priority: errors first, then reads, then writes
        if (has_error) {
            errors.push_back(fd);
        } else if (has_read) {
            readable.push_back(fd);
        } else if (has_write) {
            writable.push_back(fd);
        } else {
            // Unknown event type - log and treat as error
            LOG_WARN("BatchIOHandler::GroupEvents unknown event type 0x%x for fd=%d",
                     revents, fd);
            errors.push_back(fd);
        }
    }

    // Remove duplicates (in case an fd has multiple event types)
    // This shouldn't happen with proper epoll usage, but handle it anyway
    auto remove_duplicates = [](std::vector<int>& vec) {
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    };

    remove_duplicates(readable);
    remove_duplicates(writable);
    remove_duplicates(errors);

    // Remove fds from readable/writable if they're also in errors
    auto remove_if_in_errors = [&errors](std::vector<int>& vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
            [&errors](int fd) {
                return std::find(errors.begin(), errors.end(), fd) != errors.end();
            }),
            vec.end());
    };

    remove_if_in_errors(readable);
    remove_if_in_errors(writable);

    LOG_DEBUG("BatchIOHandler::GroupEvents: %zu readable, %zu writable, %zu errors",
              readable.size(), writable.size(), errors.size());
}