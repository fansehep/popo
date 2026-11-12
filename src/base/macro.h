#pragma once

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>

#include <cpptrace/cpptrace.hpp>

#define PANIC(str, ...) \
    do \
    { \
        cpptrace::generate_trace().print(); \
        std::cout << std::format("[{}:{}] panic with: " str "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        std::abort(); \
    } while (0)


#define ASSERT(cond, str, ...) \
    do \
    { \
        if (!(cond)) \
        { \
            cpptrace::generate_trace().print(); \
            std::cout << std::format("[{}:{}] assert fail: " str "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)


inline std::string SystemCallErr()
{
    return std::format("[{}:{}]", errno, std::strerror(errno));
}

namespace popo::base
{

inline void signal_handler(int signal)
{
    cpptrace::generate_trace().print();
    std::cout << std::format("Received signal: {}\n", signal);
    std::exit(signal);
}

inline void setup_signal_handlers()
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGSEGV, signal_handler);
    std::signal(SIGABRT, signal_handler);
    std::signal(SIGILL, signal_handler);
}

__attribute__((constructor)) static void initialize()
{
    setup_signal_handlers();
}

} // namespace popo::base
