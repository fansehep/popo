cmake_minimum_required(VERSION 3.20)

project(Neon LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(CMAKE_CXX_STANDARD 23)

add_compile_options(-g "-O0")
include(FetchContent)


# 禁用 CMake 对 C++ 模块的自动扫描（如果你不使用模块或想避免模块相关生成器限制）
set(CMAKE_CXX_SCAN_FOR_MODULES 0)

FetchContent_Declare(
        cpptrace
        URL https://github.com/jeremy-rifkin/cpptrace/archive/refs/tags/v0.7.1.tar.gz
)
FetchContent_MakeAvailable(cpptrace)

FetchContent_Declare(
        spdlog
        URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.0.tar.gz
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
)
FetchContent_MakeAvailable(asio)

# Create an interface target for asio
add_library(asio_lib INTERFACE)
target_include_directories(asio_lib INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio_lib INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
# asio needs pthread
find_package(Threads REQUIRED)
target_link_libraries(asio_lib INTERFACE Threads::Threads)

add_subdirectory(src)
add_subdirectory(tests)