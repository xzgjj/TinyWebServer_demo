//

#pragma once
#include <unistd.h>
#include <vector>
#include <cstddef>

enum class ConnState {
    OPEN,
    CLOSING,
    CLOSED
};


class Connection {
public:
    explicit Connection(int fd);
    ~Connection();

    int Fd() const noexcept;
    ConnState State() const noexcept;
    bool HasPendingWrite() const { return !write_buffer_.empty(); };

    void HandleRead();
    void HandleWrite();
    bool TryFlushWriteBuffer();
    void Close();

private:
    int fd_;
    ConnState state_;
    std::vector<char> read_buffer_;
    std::vector<char> write_buffer_;
};