#pragma once

#include <format>
#include <errno.h>
#include <iostream>
#include <format>
#include <cstring>
#include <cpptrace/cpptrace.hpp>

#define PANIC(str, ...) \
    do \
    { \
        cpptrace::generate_trace().print(); \
        std::cout << std::format("[{}:{}] panic with: " str "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        abort(); \
    } while (0) \


#define ASSERT(cond, str, ...) \
    do \
    { \
        if (!(cond)) \
        { \
            cpptrace::generate_trace().print(); \
            std::cout << std::format("[{}:{}] assert fail: " str "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0) \


static std::string SystemCallErr() {
    return std::format("[{}:{}]", errno, strerror(errno));
}

#include <csignal>

namespace neon::base
{

static void signal_handler(int signal)
{
    
    cpptrace::generate_trace().print();
    std::cout << std::format("Received signal: {}\n", signal);
    std::exit(signal);
}

static void setup_signal_handlers()
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGSEGV, signal_handler);
    std::signal(SIGABRT, signal_handler);
    std::signal(SIGILL, signal_handler);
}

__attribute__((constructor))
static void initialize()
{
    setup_signal_handlers();
}
}