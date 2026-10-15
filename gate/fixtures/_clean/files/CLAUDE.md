# 夹具项目说明

本目录是门禁自测的正向夹具：所有规则都不应该在这里报红。

## 构建

使用 `CMakeLists.txt` 配置，标准为 C++23。

## 目录

- `src/main.cpp`：唯一的核心源文件。
- `test/test_basic.cpp`：对应的 GTest 用例，已注册进 CMake。
- `gate/spec.toml`：由自测框架渲染出的规格文件。
