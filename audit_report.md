# TinyWebServer 自动化审计与性能报告

- **生成时间:** 2026-03-07 19:32:42
- **项目状态:** ✅ 全部通过
- **测试概览:** 通过 15, 失败 0

## 详细测试概览
| 测试项 | 状态 | 关键指标/错误摘要 | 详细日志 |
| :--- | :--- | :--- | :--- |
| test_structured_log | ✅ 通过 | N/A | [Log](./build/test_logs/test_structured_log.log) |
| test_request_validator | ✅ 通过 | Success | [Log](./build/test_logs/test_request_validator.log) |
| test_log | ✅ 通过 | RESULT: 50774.89 logs/sec | Total Time: 0.7878s | [Log](./build/test_logs/test_log.log) |
| test_main | ✅ 通过 | Success | [Log](./build/test_logs/test_main.log) |
| test_conditional_request | ✅ 通过 | Success | [Log](./build/test_logs/test_conditional_request.log) |
| test_single_connection | ✅ 通过 | N/A | [Log](./build/test_logs/test_single_connection.log) |
| test_lifecycle | ✅ 通过 | N/A | [Log](./build/test_logs/test_lifecycle.log) |
| test_timer | ✅ 通过 | N/A | [Log](./build/test_logs/test_timer.log) |
| test_multithread_reactor | ✅ 通过 | N/A | [Log](./build/test_logs/test_multithread_reactor.log) |
| test_stress | ✅ 通过 | RESULT: QPS: 0.00 | Latency: 0.00ms | Total: 0 | [Log](./build/test_logs/test_stress.log) |
| test_basic | ✅ 通过 | N/A | [Log](./build/test_logs/test_basic.log) |
| test_keep_alive | ✅ 通过 | Success | [Log](./build/test_logs/test_keep_alive.log) |
| test_backpressure | ✅ 通过 | N/A | [Log](./build/test_logs/test_backpressure.log) |
| test_multi_connection | ✅ 通过 | N/A | [Log](./build/test_logs/test_multi_connection.log) |
| test_client_close | ✅ 通过 | N/A | [Log](./build/test_logs/test_client_close.log) |


---
*注：性能指标从标准输出中实时提取。若出现失败项(-6)，请检查日志中的 ASan 内存审计报告。*