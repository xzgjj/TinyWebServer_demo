# Diff 记录 - TinyWebServer 关键代码修改

## 2026-03-06 22:03:38
### 涉及文件
1. `implementation_plan.md` (新建)
2. `CLAUDE.md` (更新)
3. `notes.txt` (更新)

### 核心 Diff 摘要
#### 1. 工程化实现规划创建
```diff
+ # TinyWebServer 工程级实现规划 (v6.4+)
+
+ ## 项目目标
+ 将当前 Reactor Web Server Demo 升级为**工程级可用的准生产系统**...
+
+ ## 当前状态评估 (v6-multi-reactor)
+ ### 已具备的核心能力
+ 1. **Reactor 架构**：Main-Sub Reactor 模型...
+
+ ### 与工程级的5点关键差距
+ 1. **Connection 状态机不完整**...
+ 2. **资源管理与边界控制缺失**...
+ 3. **错误处理碎片化**...
+ 4. **可观测性不足**...
+ 5. **配置系统缺失**...
+
+ ## 完整实现规划 (5个阶段)
+ ### 阶段一：核心架构升级 (v6.1)
+ - Connection 完整状态机 (7个状态)
+ - 资源边界控制与背压机制
+
+ ### 阶段二：错误处理与超时管理 (v6.2)
+ - 统一错误码体系 (WebError 枚举)
+ - 定时器管理器 (时间轮)
+
+ ### 阶段三：配置与可观测性 (v6.3)
+ - JSON 配置文件系统 + 热重载
+ - 监控指标收集 (Prometheus 格式)
+ - 结构化日志增强
+
+ ### 阶段四：安全与协议完整性 (v6.4)
+ - 请求安全验证 (路径遍历防护)
+ - HTTP/1.1 完整特性 (Keep-Alive, 条件请求)
+ - 简单反向代理模块
+
+ ### 阶段五：性能优化与高级特性 (v6.5+)
+ - 内存池优化
+ - I/O 多队列支持
+ - 插件系统框架
```

#### 2. 项目文档更新
```diff
# CLAUDE.md 更新
+ ## 工程化演进规划 (v6.1-v6.5+)
+ 已制定详细的5阶段实施规划...
+
+ ### 阶段目标
+ 1. **v6.1**：Connection 完整状态机...
+
+ ### 核心约束增强
+ 6. **状态机完整性**：Connection 必须实现完整状态机...
+ 7. **资源边界**：内存、连接数、缓冲区必须有明确上限...

# notes.txt 更新
+ ### 新的工程化规划 (2026-03-06)
+ **已创建详细实现规划文件**：`implementation_plan.md`
+ - **阶段一**：Connection 完整状态机 + 资源边界控制 (v6.1)...
```

### 修改意图
1. **系统化工程升级**：解决当前 Demo 与工程级实现的5点关键差距
2. **渐进式实施**：制定5个阶段的详细规划，降低重构风险
3. **文档完整性**：确保代码、设计、规划文档同步更新
4. **明确目标**：为v6.4+版本设定清晰的工程化里程碑
5. **保持教学价值**：复杂功能作为可选模块，核心保持简洁可理解

---

## 2026-03-06 21:38:23
### 涉及文件
1. `.github/workflows/ci.yml`
2. `CMakeLists.txt`

### 核心 Diff 摘要
#### 1. CI 流水线重构
```diff
- name: C++ CI & Memory Check
+ name: C++ CI

- jobs:
-   build-and-test:
-     runs-on: ubuntu-latest
-     steps: [...单一任务，同时运行测试和Valgrind]
+ jobs:
+   build-and-test:
+     name: Build + ASan Smoke Tests
+     runs-on: ubuntu-latest
+     steps: [...使用ASan运行冒烟测试]
+
+   memcheck:
+     name: Valgrind Memcheck
+     runs-on: ubuntu-latest
+     needs: build-and-test
+     steps: [...禁用ASan，使用Valgrind检查内存泄漏]
```

#### 2. CMake 测试分类管理
```diff
# 旧：简单添加测试可执行文件
- foreach(test_name ${ALL_TEST_TARGETS})
-   add_executable(${test_name} ...)
- endforeach()

# 新：CTest 集成 + 测试分类
+ if(BUILD_TESTING)
+   # 定义冒烟测试集
+   set(CI_SMOKE_TESTS test_timer test_basic test_multithread_reactor test_log)
+
+   foreach(test_name ${ALL_TEST_TARGETS})
+     add_executable(${test_name} ...)
+     add_test(NAME ${test_name} COMMAND ${test_name})
+     set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
+
+     if(test_name IN_LIST CI_SMOKE_TESTS)
+       set_tests_properties(${test_name} PROPERTIES LABELS "smoke;integration")
+     elseif(...)
+       # 其他分类
+     endif()
+   endforeach()
+ endif()
```

### 修改意图
1. **分离 ASan 与 Valgrind**：两者不能同时使用，拆分任务可充分发挥各自优势
2. **加速 CI 反馈**：冒烟测试（~30秒）快速验证基本功能，内存检查作为深度验证
3. **提高可维护性**：测试分类（smoke/integration/benchmark）便于选择性运行
4. **错误隔离**：Valgrind 日志单独保存，内存问题分析更清晰
5. **标准化测试**：通过 CTest 集成，支持 `ctest -L smoke` 等标准命令

---

## 修改记录模板
```
## YYYY-MM-DD HH:MM:SS
### 涉及文件
- file1.cpp
- file2.h

### 核心 Diff 摘要
```diff
// 用简短的代码块展示关键修改
- 旧代码
+ 新代码
```

### 修改意图
1. 为什么修改（问题背景）
2. 修改解决了什么
3. 预期效果
```

> **注意**：每次重要修改后更新此文件，保持增量记录，不删除历史记录。