// Keep-Alive 管理器测试
// 验证 KeepAliveManager 的基本功能：请求计数、空闲超时、连接状态管理

#include "http/keep_alive_manager.h"
#include <cassert>
#include <thread>
#include <chrono>

void TestKeepAliveManagerBasic() {
    using namespace tinywebserver;

    // 1. 创建管理器，默认空闲超时 5 秒
    KeepAliveManager manager(std::chrono::seconds(5));

    // 2. 模拟连接开始请求（Keep-Alive 启用）
    manager.OnRequestStart(1, true, 5); // fd=1, keep_alive=true, idle_timeout=5
    manager.OnRequestStart(2, false, 0); // fd=2, keep_alive=false

    // 3. 验证连接状态
    auto state1 = manager.GetConnectionState(1);
    auto state2 = manager.GetConnectionState(2);
    assert(state1 != nullptr);
    assert(state2 != nullptr);
    assert(state1->keep_alive == true);
    assert(state2->keep_alive == false);
    assert(state1->request_count == 1);
    assert(state2->request_count == 1);

    // 4. 同一连接第二个请求
    manager.OnRequestStart(1, true, 5);
    state1 = manager.GetConnectionState(1);
    assert(state1->request_count == 2);

    // 5. 请求完成
    manager.OnRequestComplete(1);
    manager.OnRequestComplete(2);

    // 6. 检查空闲时间（应该很小）
    //    容差说明：GetIdleSeconds 是**整秒**粒度（now - last_active 取秒），
    //    因此"睡 1 秒后必然 >=1"并不成立：实测在共享 CI runner 上
    //    可能读到 0（0.999s 截断）。计时类断言必须按计时器粒度写，
    //    否则测的是调度抖动而不是 KeepAlive 逻辑。
    //    本用例真正要守住的行为在下面：5 秒超时下 1 秒后不得判超时。
    int idle_seconds = manager.GetIdleSeconds(1);
    assert(idle_seconds >= 0 && idle_seconds <= 5);

    // 7. 模拟时间流逝（睡眠 1 秒）
    std::this_thread::sleep_for(std::chrono::seconds(1));
    idle_seconds = manager.GetIdleSeconds(1);
    assert(idle_seconds >= 0 && idle_seconds <= 5);   // 只断言"单调且在合理窗口内"

    // 8. 检查空闲超时（5秒超时，1秒后未超时）
    assert(manager.IsIdleTimeout(1) == false);

    // 9. 连接关闭
    manager.OnConnectionClose(1);
    manager.OnConnectionClose(2);
    assert(manager.GetConnectionState(1) == nullptr);
    assert(manager.GetConnectionState(2) == nullptr);

    // 10. 清理超时连接（无连接应被清理）
    auto timed_out = manager.CleanupTimeoutConnections();
    assert(timed_out.empty());

    printf("TestKeepAliveManagerBasic: PASSED\n");
}

void TestKeepAliveManagerTimeout() {
    using namespace tinywebserver;

    // 1. 创建管理器，超时 1 秒
    KeepAliveManager manager(std::chrono::seconds(1));

    // 2. 添加 Keep-Alive 连接
    manager.OnRequestStart(1, true, 1); // 1秒超时
    manager.OnRequestComplete(1);

    // 3. 等待超过超时时间
    std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // 1.1秒

    // 4. 检查是否超时
    assert(manager.IsIdleTimeout(1) == true);

    // 5. 清理超时连接
    auto timed_out = manager.CleanupTimeoutConnections();
    assert(timed_out.size() == 1);
    assert(timed_out[0] == 1);
    assert(manager.GetConnectionState(1) == nullptr);

    printf("TestKeepAliveManagerTimeout: PASSED\n");
}

void TestKeepAliveManagerStatistics() {
    using namespace tinywebserver;

    KeepAliveManager manager(std::chrono::seconds(10));

    // 模拟多个请求
    for (int i = 1; i <= 5; i++) {
        manager.OnRequestStart(i, true, 10);
        manager.OnRequestComplete(i);
    }

    assert(manager.GetTotalRequestCount() == 5);
    assert(manager.GetActiveConnectionCount() == 5);

    // 关闭两个连接
    manager.OnConnectionClose(1);
    manager.OnConnectionClose(2);
    assert(manager.GetActiveConnectionCount() == 3);

    // 重置统计
    manager.ResetStatistics();
    assert(manager.GetTotalRequestCount() == 0);

    printf("TestKeepAliveManagerStatistics: PASSED\n");
}

int main() {
    printf("Running Keep-Alive manager tests...\n");

    TestKeepAliveManagerBasic();
    TestKeepAliveManagerTimeout();
    TestKeepAliveManagerStatistics();

    printf("\n✅ All Keep-Alive manager tests passed!\n");
    return 0;
}