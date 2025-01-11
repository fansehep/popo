#include <format>
#include "spdlog/spdlog.h"
#include "src/type/value.h"

using namespace neon::type;

int main()
{
    {
        Value val(ValueType::Int8, static_cast<int8_t>(12));
        Value val2(ValueType::String, "123");
        SPDLOG_INFO("value:{}", val.i8());
    }
    {
        SPDLOG_INFO("value len:{}", sizeof(Value));
    }
    {
        std::array<int, 3> arr{};
        SPDLOG_INFO("{}", arr.size());
    }
}