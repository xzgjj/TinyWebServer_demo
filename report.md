# TinyWebServer V3 自动化审计报告

- **生成时间:** 2026-01-20 03:14:50
- **运行时长:** 43.2 秒
- **模式:** Release (-O3) + AddressSanitizer
- **状态:** ⚠️ 测试被用户中断
- **统计:** 9 个测试，4 通过，5 失败，0 其他

## 1. 编译状态
状态: 未记录编译日志

## 2. 测试概览
| 测试项 | 状态 | 详细链接 |
| :--- | :--- | :--- |
| test_lifecycle | ✅ PASS | [查看详情](#test_lifecycle) |
| test_single_connection | ❌ FAIL | [查看详情](#test_single_connection) |
| test_multi_connection | ❌ FAIL | [查看详情](#test_multi_connection) |
| test_client_close | ❌ FAIL | [查看详情](#test_client_close) |
| test_backpressure | ❌ FAIL | [查看详情](#test_backpressure) |
| test_stress | ❌ FAIL | [查看详情](#test_stress) |
| test_main | ✅ PASS | [查看详情](#test_main) |
| test_multithread_reactor | ✅ PASS | [查看详情](#test_multithread_reactor) |
| test_basic | ✅ PASS | [查看详情](#test_basic) |

## 3. 详细输出
### <a name="test_lifecycle"></a>test_lifecycle
**状态:** PASS

```text
[Server] TinyWebServer V3 (FSM Protocol) on port 8080
[Server] TinyWebServer V3 is running...
[PASS] Server lifecycle started successfully (PID: 25897).

```

### <a name="test_single_connection"></a>test_single_connection
**状态:** FAIL

```text
[Test] Starting Single Connection Test (HTTP V3)...
[Test] Sending HTTP Request...

```

### <a name="test_multi_connection"></a>test_multi_connection
**状态:** FAIL

```text
[Test] Starting HTTP Multi-Connection Test (20 clients)...

```

### <a name="test_client_close"></a>test_client_close
**状态:** FAIL

```text
无输出
```

### <a name="test_backpressure"></a>test_backpressure
**状态:** FAIL

```text


[Valgrind Analysis]
==25910== Memcheck, a memory error detector
==25910== Copyright (C) 2002-2022, and GNU GPL'd, by Julian Seward et al.
==25910== Using Valgrind-3.22.0 and LibVEX; rerun with -h for copyright info
==25910== Command: /mnt/d/vsc_project/TinyWebServer_demo/build/test_backpressure
==25910== Parent PID: 25894
==25910== 
==25910== 
==25910== Process terminating with default action of signal 13 (SIGPIPE)
==25910==    at 0x4C21920: send (send.c:28)
==25910==    by 0x109219: main (test_backpressure.cpp:15)
==25910== 
==25910== HEAP SUMMARY:
==25910==     in use at exit: 1,122,305 bytes in 2 blocks
==25910==   total heap usage: 2 allocs, 0 frees, 1,122,305 bytes allocated
==25910== 
==25910== LEAK SUMMARY:
==25910==    definitely lost: 0 bytes in 0 blocks
==25910==    indirectly lost: 0 bytes in 0 blocks
==25910==      possibly lost: 0 bytes in 0 blocks
==25910==    still reachable: 1,122,305 bytes in 2 blocks
==25910==         suppressed: 0 bytes in 0 blocks
==25910== Reachable blocks (those to which a pointer was found) are not shown.
==25910== To see them, rerun with: --leak-check=full --show-leak-kinds=all
==25910== 
==25910== For lists of detected and suppressed errors, rerun with: -s
==25910== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

```

### <a name="test_stress"></a>test_stress
**状态:** FAIL

```text
[Test] Starting HTTP Stress Test (100 rounds)...


[Valgrind Analysis]
==25912== Memcheck, a memory error detector
==25912== Copyright (C) 2002-2022, and GNU GPL'd, by Julian Seward et al.
==25912== Using Valgrind-3.22.0 and LibVEX; rerun with -h for copyright info
==25912== Command: /mnt/d/vsc_project/TinyWebServer_demo/build/test_stress
==25912== Parent PID: 25894
==25912== 
==25912== 
==25912== Process terminating with default action of signal 13 (SIGPIPE)
==25912==    at 0x4C21920: send (send.c:28)
==25912==    by 0x10963D: main (test_stress.cpp:17)
==25912== 
==25912== HEAP SUMMARY:
==25912==     in use at exit: 77,855 bytes in 3 blocks
==25912==   total heap usage: 3 allocs, 0 frees, 77,855 bytes allocated
==25912== 
==25912== LEAK SUMMARY:
==25912==    definitely lost: 0 bytes in 0 blocks
==25912==    indirectly lost: 0 bytes in 0 blocks
==25912==      possibly lost: 0 bytes in 0 blocks
==25912==    still reachable: 77,855 bytes in 3 blocks
==25912==         suppressed: 0 bytes in 0 blocks
==25912== Reachable blocks (those to which a pointer was found) are not shown.
==25912== To see them, rerun with: --leak-check=full --show-leak-kinds=all
==25912== 
==25912== For lists of detected and suppressed errors, rerun with: -s
==25912== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

```

### <a name="test_main"></a>test_main
**状态:** PASS

```text
Running TinyWebServer tests...
EventLoop created successfully
Server created successfully

```

### <a name="test_multithread_reactor"></a>test_multithread_reactor
**状态:** PASS

```text
Testing EventLoopThreadPool...
Distinct IO threads count: 3
EventLoopThreadPool test passed!

```

### <a name="test_basic"></a>test_basic
**状态:** PASS

```text
无输出
```

## 4. 总结与建议
### ❌ 发现问题
- 以下测试失败: test_single_connection, test_multi_connection, test_client_close, test_backpressure, test_stress
- 建议检查网络连接、端口冲突或服务器配置
### ⏸️ 测试被中断
- 用户按下了 Ctrl+C
- 部分测试可能没有完成
- 建议重新运行完整的测试流程
