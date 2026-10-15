#include "reactor/multi_listen_socket.h"
#include "Logger.h"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

class MultiListenSocketTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 设置日志级别为 WARN 以减少测试输出
        Logger::GetInstance().SetMinLevel(LogLevel::LOG_LEVEL_WARN);
    }

    void TearDown() override {
        // 清理
    }
};

// 测试基本创建和销毁
TEST_F(MultiListenSocketTest, BasicCreation) {
    // 跳过测试如果系统不支持 SO_REUSEPORT
    if (!MultiListenSocket::IsSOReusePortSupported()) {
        GTEST_SKIP() << "SO_REUSEPORT not supported on this system";
    }

    MultiListenSocket socket("127.0.0.1", 0, 2); // 端口0表示由系统分配
    EXPECT_TRUE(socket.IsValid());
    EXPECT_EQ(socket.GetNumSockets(), 2);
    EXPECT_TRUE(socket.GetError().empty());
}

// 测试无效参数
TEST_F(MultiListenSocketTest, InvalidParameters) {
    // 0个socket应该失败
    MultiListenSocket socket("127.0.0.1", 8080, 0);
    EXPECT_FALSE(socket.IsValid());
    EXPECT_FALSE(socket.GetError().empty());
}

// 测试获取socket文件描述符
TEST_F(MultiListenSocketTest, GetSocketFd) {
    if (!MultiListenSocket::IsSOReusePortSupported()) {
        GTEST_SKIP() << "SO_REUSEPORT not supported on this system";
    }

    MultiListenSocket socket("127.0.0.1", 0, 3);
    ASSERT_TRUE(socket.IsValid());

    // 测试有效索引
    int fd0 = socket.GetSocketFd(0);
    int fd1 = socket.GetSocketFd(1);
    int fd2 = socket.GetSocketFd(2);

    EXPECT_GT(fd0, 0);
    EXPECT_GT(fd1, 0);
    EXPECT_GT(fd2, 0);
    EXPECT_NE(fd0, fd1); // 应该是不同的socket

    // 测试无效索引
    EXPECT_EQ(socket.GetSocketFd(3), -1);
    EXPECT_EQ(socket.GetSocketFd(100), -1);
}

// 测试移动语义
TEST_F(MultiListenSocketTest, MoveSemantics) {
    if (!MultiListenSocket::IsSOReusePortSupported()) {
        GTEST_SKIP() << "SO_REUSEPORT not supported on this system";
    }

    MultiListenSocket socket1("127.0.0.1", 0, 2);
    ASSERT_TRUE(socket1.IsValid());
    size_t num_sockets = socket1.GetNumSockets();

    // 移动构造
    MultiListenSocket socket2(std::move(socket1));
    EXPECT_FALSE(socket1.IsValid());
    EXPECT_TRUE(socket2.IsValid());
    EXPECT_EQ(socket2.GetNumSockets(), num_sockets);

    // 移动赋值
    MultiListenSocket socket3("127.0.0.1", 0, 1);
    socket3 = std::move(socket2);
    EXPECT_FALSE(socket2.IsValid());
    EXPECT_TRUE(socket3.IsValid());
    EXPECT_EQ(socket3.GetNumSockets(), num_sockets);
}

// 测试 GetAllSocketFds
TEST_F(MultiListenSocketTest, GetAllSocketFds) {
    if (!MultiListenSocket::IsSOReusePortSupported()) {
        GTEST_SKIP() << "SO_REUSEPORT not supported on this system";
    }

    const size_t num_sockets = 4;
    MultiListenSocket socket("127.0.0.1", 0, num_sockets);
    ASSERT_TRUE(socket.IsValid());

    const auto& fds = socket.GetAllSocketFds();
    EXPECT_EQ(fds.size(), num_sockets);

    // 检查所有fd都是有效的
    for (int fd : fds) {
        EXPECT_GT(fd, 0);
    }

    // 检查没有重复的fd
    std::vector<int> unique_fds = fds;
    std::sort(unique_fds.begin(), unique_fds.end());
    unique_fds.erase(std::unique(unique_fds.begin(), unique_fds.end()), unique_fds.end());
    EXPECT_EQ(unique_fds.size(), fds.size());
}

// 测试 SO_REUSEPORT 支持检测
TEST_F(MultiListenSocketTest, SOReusePortSupport) {
    // 这个测试只是检查函数是否正常工作
    bool supported = MultiListenSocket::IsSOReusePortSupported();
    (void)supported; // 防止未使用变量警告
    // 无论结果如何，函数应该正常返回
    // 在支持的系统上为true，在不支持的系统上为false
    SUCCEED();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}