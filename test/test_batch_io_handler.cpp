#include "reactor/batch_io_handler.h"
#include "Logger.h"
#include <gtest/gtest.h>
#include <vector>
#include <atomic>

class BatchIOHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::GetInstance().SetMinLevel(LogLevel::LOG_LEVEL_WARN);
    }

    BatchIOHandler handler;
};

// 测试空事件处理
TEST_F(BatchIOHandlerTest, EmptyEvents) {
    std::vector<epoll_event> events;
    std::atomic<int> read_count{0}, write_count{0}, error_count{0};

    auto result = handler.ProcessBatch(
        events,
        [&](int fd) { read_count++; },
        [&](int fd) { write_count++; },
        [&](int fd) { error_count++; }
    );

    EXPECT_EQ(result.total_processed, 0);
    EXPECT_EQ(result.read_processed, 0);
    EXPECT_EQ(result.write_processed, 0);
    EXPECT_EQ(result.error_processed, 0);
    EXPECT_EQ(read_count, 0);
    EXPECT_EQ(write_count, 0);
    EXPECT_EQ(error_count, 0);
}

// 测试单个读事件
TEST_F(BatchIOHandlerTest, SingleReadEvent) {
    std::vector<epoll_event> events(1);
    events[0].data.fd = 42;
    events[0].events = EPOLLIN;

    std::atomic<int> read_count{0}, write_count{0}, error_count{0};
    std::vector<int> read_fds;

    auto result = handler.ProcessBatch(
        events,
        [&](int fd) { read_count++; read_fds.push_back(fd); },
        [&](int fd) { write_count++; },
        [&](int fd) { error_count++; }
    );

    EXPECT_EQ(result.total_processed, 1);
    EXPECT_EQ(result.read_processed, 1);
    EXPECT_EQ(result.write_processed, 0);
    EXPECT_EQ(result.error_processed, 0);
    EXPECT_EQ(read_count, 1);
    EXPECT_EQ(write_count, 0);
    EXPECT_EQ(error_count, 0);
    EXPECT_EQ(read_fds.size(), 1);
    EXPECT_EQ(read_fds[0], 42);
}

// 测试单个写事件
TEST_F(BatchIOHandlerTest, SingleWriteEvent) {
    std::vector<epoll_event> events(1);
    events[0].data.fd = 43;
    events[0].events = EPOLLOUT;

    std::atomic<int> read_count{0}, write_count{0}, error_count{0};
    std::vector<int> write_fds;

    auto result = handler.ProcessBatch(
        events,
        [&](int fd) { read_count++; },
        [&](int fd) { write_count++; write_fds.push_back(fd); },
        [&](int fd) { error_count++; }
    );

    EXPECT_EQ(result.total_processed, 1);
    EXPECT_EQ(result.read_processed, 0);
    EXPECT_EQ(result.write_processed, 1);
    EXPECT_EQ(result.error_processed, 0);
    EXPECT_EQ(read_count, 0);
    EXPECT_EQ(write_count, 1);
    EXPECT_EQ(error_count, 0);
    EXPECT_EQ(write_fds.size(), 1);
    EXPECT_EQ(write_fds[0], 43);
}

// 测试单个错误事件
TEST_F(BatchIOHandlerTest, SingleErrorEvent) {
    std::vector<epoll_event> events(1);
    events[0].data.fd = 44;
    events[0].events = EPOLLERR;

    std::atomic<int> read_count{0}, write_count{0}, error_count{0};
    std::vector<int> error_fds;

    auto result = handler.ProcessBatch(
        events,
        [&](int fd) { read_count++; },
        [&](int fd) { write_count++; },
        [&](int fd) { error_count++; error_fds.push_back(fd); }
    );

    EXPECT_EQ(result.total_processed, 1);
    EXPECT_EQ(result.read_processed, 0);
    EXPECT_EQ(result.write_processed, 0);
    EXPECT_EQ(result.error_processed, 1);
    EXPECT_EQ(read_count, 0);
    EXPECT_EQ(write_count, 0);
    EXPECT_EQ(error_count, 1);
    EXPECT_EQ(error_fds.size(), 1);
    EXPECT_EQ(error_fds[0], 44);
}

// 测试混合事件
TEST_F(BatchIOHandlerTest, MixedEvents) {
    std::vector<epoll_event> events(6);
    events[0].data.fd = 1; events[0].events = EPOLLIN;
    events[1].data.fd = 2; events[1].events = EPOLLOUT;
    events[2].data.fd = 3; events[2].events = EPOLLERR;
    events[3].data.fd = 4; events[3].events = EPOLLIN | EPOLLOUT; // 读和写
    events[4].data.fd = 5; events[4].events = EPOLLIN | EPOLLERR; // 读和错误（错误优先）
    events[5].data.fd = 6; events[5].events = EPOLLHUP; // 挂起

    std::atomic<int> read_count{0}, write_count{0}, error_count{0};
    std::vector<int> read_fds, write_fds, error_fds;

    auto result = handler.ProcessBatch(
        events,
        [&](int fd) { read_count++; read_fds.push_back(fd); },
        [&](int fd) { write_count++; write_fds.push_back(fd); },
        [&](int fd) { error_count++; error_fds.push_back(fd); }
    );

    EXPECT_EQ(result.total_processed, 6);

    // 期望：fd1(读), fd2(写), fd3(错误), fd4(读和写->读), fd5(读和错误->错误), fd6(挂起->错误)
    EXPECT_EQ(read_count, 2);   // fd1, fd4
    EXPECT_EQ(write_count, 1);  // fd2
    EXPECT_EQ(error_count, 3);  // fd3, fd5, fd6

    // 检查具体的fd
    EXPECT_EQ(read_fds.size(), 2);
    EXPECT_EQ(write_fds.size(), 1);
    EXPECT_EQ(error_fds.size(), 3);

    // fd1和fd4应该在读列表中
    EXPECT_TRUE(std::find(read_fds.begin(), read_fds.end(), 1) != read_fds.end());
    EXPECT_TRUE(std::find(read_fds.begin(), read_fds.end(), 4) != read_fds.end());

    // fd2应该在写列表中
    EXPECT_TRUE(std::find(write_fds.begin(), write_fds.end(), 2) != write_fds.end());

    // fd3, fd5, fd6应该在错误列表中
    EXPECT_TRUE(std::find(error_fds.begin(), error_fds.end(), 3) != error_fds.end());
    EXPECT_TRUE(std::find(error_fds.begin(), error_fds.end(), 5) != error_fds.end());
    EXPECT_TRUE(std::find(error_fds.begin(), error_fds.end(), 6) != error_fds.end());
}

// 测试重复fd处理（相同fd出现在多个事件中）
TEST_F(BatchIOHandlerTest, DuplicateFd) {
    std::vector<epoll_event> events(3);
    events[0].data.fd = 10; events[0].events = EPOLLIN;
    events[1].data.fd = 10; events[1].events = EPOLLOUT; // 同一个fd，不同事件
    events[2].data.fd = 10; events[2].events = EPOLLERR; // 同一个fd，错误事件

    std::atomic<int> read_count{0}, write_count{0}, error_count{0};

    auto result = handler.ProcessBatch(
        events,
        [&](int fd) { read_count++; },
        [&](int fd) { write_count++; },
        [&](int fd) { error_count++; }
    );

    // 错误优先，所以应该只有错误回调被调用
    EXPECT_EQ(result.total_processed, 3);
    EXPECT_EQ(result.error_processed, 1); // 错误事件
    EXPECT_EQ(result.read_processed, 0);
    EXPECT_EQ(result.write_processed, 0);
    EXPECT_EQ(error_count, 1);
    EXPECT_EQ(read_count, 0);
    EXPECT_EQ(write_count, 0);
}

// 测试 BatchRead 方法
TEST_F(BatchIOHandlerTest, BatchReadMethod) {
    std::vector<int> fds = {1, 2, 3, 4, 5};
    std::atomic<int> count{0};
    std::vector<int> processed_fds;

    handler.BatchRead(fds, [&](int fd) {
        count++;
        processed_fds.push_back(fd);
    });

    EXPECT_EQ(count, 5);
    EXPECT_EQ(processed_fds.size(), 5);

    // 检查所有fd都被处理了
    for (int fd : fds) {
        EXPECT_TRUE(std::find(processed_fds.begin(), processed_fds.end(), fd) != processed_fds.end());
    }
}

// 测试 BatchWrite 方法
TEST_F(BatchIOHandlerTest, BatchWriteMethod) {
    std::vector<int> fds = {10, 20, 30};
    std::atomic<int> count{0};

    handler.BatchWrite(fds, [&](int fd) {
        count++;
    });

    EXPECT_EQ(count, 3);
}

// 测试 BatchError 方法
TEST_F(BatchIOHandlerTest, BatchErrorMethod) {
    std::vector<int> fds = {100, 200};
    std::atomic<int> count{0};

    handler.BatchError(fds, [&](int fd) {
        count++;
    });

    EXPECT_EQ(count, 2);
}

// 测试异常安全的回调
TEST_F(BatchIOHandlerTest, ExceptionSafety) {
    std::vector<epoll_event> events(3);
    events[0].data.fd = 1; events[0].events = EPOLLIN;
    events[1].data.fd = 2; events[1].events = EPOLLIN;
    events[2].data.fd = 3; events[2].events = EPOLLIN;

    std::atomic<int> count{0};

    // 第二个回调会抛出异常
    auto result = handler.ProcessBatch(
        events,
        [&](int fd) {
            count++;
            if (fd == 2) {
                throw std::runtime_error("Test exception");
            }
        },
        [&](int fd) { },
        [&](int fd) { }
    );

    // 即使有异常，所有事件都应该被处理
    EXPECT_EQ(result.total_processed, 3);
    EXPECT_EQ(result.read_processed, 3);
    // count应该是3，因为异常被捕获了，处理继续
    EXPECT_EQ(count, 3);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}