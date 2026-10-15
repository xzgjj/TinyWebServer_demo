//单/多连接基础测试 (test_basic.cpp)
//  图： 验证 socket_utils 创建监听套接字及非阻塞配置的正确性。

/**
 * @file test_basic.cpp
 * @brief 验证底层 Socket 工具类
 */

#include "reactor/socket_utils.h"
#include <cassert>
#include <unistd.h>
#include <fcntl.h>        // 修复 F_GETFL, fcntl, O_NONBLOCK
#include "Logger.h"       // 修复 LOG_INFO (请根据实际路径调整)

void TestSocketUtils() 
{
    // 1. 测试创建监听 Socket
    int listen_fd = CreateListenSocket(9090);
    assert(listen_fd > 0);
    
    // 2. 验证非阻塞属性
    int flags = fcntl(listen_fd, F_GETFL, 0);
    assert(flags & O_NONBLOCK);
    
    close(listen_fd);
    LOG_INFO("TestSocketUtils: PASSED");
}

int main() 
{
    TestSocketUtils();
    return 0;
}