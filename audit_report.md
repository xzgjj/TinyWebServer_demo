# TinyWebServer 自动化审计与性能报告

- **生成时间:** 2026-03-06 22:39:49
- **项目状态:** ❌ 存在失败项
- **测试概览:** 通过 10, 失败 1

## 详细测试概览
| 测试项 | 状态 | 关键指标/错误摘要 | 详细日志 |
| :--- | :--- | :--- | :--- |
| test_single_connection | ✅ 通过 | N/A | [Log](./build/test_logs/test_single_connection.log) |
| test_multithread_reactor | ✅ 通过 | N/A | [Log](./build/test_logs/test_multithread_reactor.log) |
| test_backpressure | ✅ 通过 | N/A | [Log](./build/test_logs/test_backpressure.log) |
| test_stress | ✅ 通过 | RESULT: QPS: 0.00 | Latency: 0.00ms | Total: 0 | [Log](./build/test_logs/test_stress.log) |
| test_lifecycle | ✅ 通过 | N/A | [Log](./build/test_logs/test_lifecycle.log) |
| test_basic | ✅ 通过 | N/A | [Log](./build/test_logs/test_basic.log) |
| test_timer | ✅ 通过 | N/A | [Log](./build/test_logs/test_timer.log) |
| test_log | ✅ 通过 | RESULT: 57854.66 logs/sec | Total Time: 0.6914s | [Log](./build/test_logs/test_log.log) |
| test_multi_connection | ✅ 通过 | N/A | [Log](./build/test_logs/test_multi_connection.log) |
| test_client_close | ✅ 通过 | N/A | [Log](./build/test_logs/test_client_close.log) |
| test_main | ❌ 失败(1) | `<span style='color:red'>[RESULT] Integration Test: FAILED</span>` | [Log](./build/test_logs/test_main.log) |


---
*注：性能指标从标准输出中实时提取。若出现失败项(-6)，请检查日志中的 ASan 内存审计报告。*