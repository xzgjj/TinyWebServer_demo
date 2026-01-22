# debug

## Build

bash
mkdir -p build
cd build
cmake ..
make

make -j4

---

## build

python3 tools.py clean

mkdir -p build
cd build

配置 CMake
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -O0 -g" \
      ..


cmake --build . --parallel

---

## debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
在一个终端（左侧）启动服务器的 GDB 调试。 
gdb ./server 


第一步：清理环境   
python3 tools.py clean
第二步：全量编译并运行所有测试
python3 tools.py all
python3 tools.py debug --target test_backpressure

python3 tools.py build
python3 tools.py test

使用 GDB 调试特定测试
python3 tools.py debug --target test_backpressure

find . -type f -exec touch {} +
终端 1 (启动服务器):  ./server
终端 2 (运行测试脚本):

sudo kill -9 $(sudo lsof -t -i:8080)
