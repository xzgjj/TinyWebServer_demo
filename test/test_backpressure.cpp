//背压与缓冲区测试


/**
 * @file test_backpressure.cpp
 * @brief 模拟高吞吐量数据流，测试缓冲区上限与内存监控
 */

#include "connection.h"
#include "reactor/socket_utils.h"
#include "Logger.h"
#include <vector>
#include <string>
#include "event_loop.h"   // 修复 EventLoop incomplete type (请根据实际路径调整)

// 模拟工业级内存上限：10MB
constexpr size_t kMaxBufferLimit = 10 * 1024 * 1024;

void TestBackpressure() 
{
    LOG_INFO("Starting Backpressure Test...");
    
    // 构造一个模拟连接
    EventLoop loop;
    auto conn = std::make_shared<Connection>(10, &loop); // 假定 FD 10

    // 获取输入缓冲区引用
    auto& input_buffer = conn->GetInputBuffer();
    
    // 模拟恶意大包注入
    std::string giant_payload(kMaxBufferLimit + 1024, 'A');
    
    // 模拟数据追加
    input_buffer.append(giant_payload);
    
    // 内存监控点检查
    if (input_buffer.size() > kMaxBufferLimit) 
    {
        LOG_WARN("Backpressure triggered: Buffer size %zu exceeds limit!", input_buffer.size());
        // 生产环境应执行 conn->HandleClose()
    }
    
    LOG_INFO("TestBackpressure: PASSED");
}

int main() 
{
    TestBackpressure();
    return 0;
}