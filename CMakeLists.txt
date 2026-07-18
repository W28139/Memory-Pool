cmake_minimum_required(VERSION 3.10)
project(MemoryPool VERSION 1.0.0 LANGUAGES CXX)

# C++ 标准要求
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 配置编译选项
if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -pthread)
endif()

# 将 include 目录加入项目的全局搜索路径
include_directories(${CMAKE_SOURCE_DIR})

# 构建静态库
add_library(MemoryPool STATIC
    src/MemoryPool.cpp
)

# 指定库的头文件位置（便于 IDE 识别）
target_include_directories(MemoryPool PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# 构建测试可执行文件
add_executable(MemoryPool_test
    tests/main.cpp
)

# 链接库
target_link_libraries(MemoryPool_test PRIVATE
    MemoryPool
)

# 启用线程支持（测试中使用了 std::thread）
if(UNIX)
    target_link_libraries(MemoryPool_test PRIVATE pthread)
endif()
