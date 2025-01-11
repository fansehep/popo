cmake_minimum_required(VERSION 3.20)

project(Neon LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(CMAKE_CXX_STANDARD 23)

add_compile_options(-g "-O0")
include(FetchContent)
FetchContent_Declare(
        cpptrace
        URL https://github.com/jeremy-rifkin/cpptrace/archive/refs/tags/v0.7.5.tar.gz
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


add_subdirectory(src)
add_subdirectory(tests)