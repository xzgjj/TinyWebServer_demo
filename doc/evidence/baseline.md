# v8 基线证据（生成于 2026-10-15 17:30:28)

## 环境
- Windows: MSYS2 UCRT64 g++ 16.2.0 / clang 22.1.8 / cmake 3.30.3 / ninja 1.13
- WSL2: Ubuntu 24.04 g++ 13.3.0 / clang 18.1.3 / cmake 3.28.3 / ninja 1.11.1 / gtest / valgrind 3.22
## 基线结论
- WSL2 g++ C++17: 构建成功，ctest 20/20 PASS（13.67s）
- Windows g++ C++17: 构建失败（sys/epoll.h, sys/uio.h, sys/mman.h, int64_t 缺 <cstdint>）
- g++ -std=c++23 全源码: 0 error
- clang++ (C++17 与 C++23 同样): 55 error（HttpRequest 歧义，预存在）
