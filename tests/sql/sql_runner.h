#pragma once

#include <chrono>
#include <stdexcept>
#include <string>

#include "src/bytecode/compiler.h"
#include "src/bytecode/vm.h"
#include "src/catalog/schema_mgr.h"
#include "src/parser/parser.h"
#include "src/planner/planner.h"
#include "src/storage/storage_engine.h"

namespace neon::test
{

struct SqlResult
{
    bool ok = false;
    std::string error;
    neon::bytecode::ExecuteSuccess result;
    int64_t parse_us = 0;
    int64_t plan_us = 0;
    int64_t compile_us = 0;
    int64_t vm_us = 0;
};

// Same pipeline as Session::handle_query: one SQL string, one statement at a time.
class SqlRunner
{
public:
    explicit SqlRunner(const std::string & data_dir, size_t buffer_pages = 4096)
        : engine_(data_dir, buffer_pages)
        , planner_(schema_)
        , compiler_(schema_)
        , vm_(schema_, engine_)
    {
        engine_.init();
    }

    SqlResult exec(const std::string & sql)
    {
        using clock = std::chrono::steady_clock;
        SqlResult out;
        try
        {
            auto t0 = clock::now();
            neon::parser::Parser parser;
            auto parsed = parser.parse(sql);
            auto t1 = clock::now();
            out.parse_us = us(t1 - t0);
            if (!parsed)
            {
                out.error = parsed.error().error_message;
                return out;
            }

            for (const auto & stmt : *parsed)
            {
                auto tp0 = clock::now();
                auto plan = planner_.plan(stmt);
                auto tp1 = clock::now();
                out.plan_us += us(tp1 - tp0);
                if (!plan)
                {
                    out.error = plan.error().error_message;
                    return out;
                }

                auto compiled = compiler_.compile(*plan);
                auto tc1 = clock::now();
                out.compile_us += us(tc1 - tp1);
                if (!compiled)
                {
                    out.error = compiled.error().error_message;
                    return out;
                }

                auto executed = vm_.execute(*compiled);
                auto tv1 = clock::now();
                out.vm_us += us(tv1 - tc1);
                if (!executed)
                {
                    out.error = executed.error().error_message;
                    return out;
                }
                out.result = std::move(*executed);
            }
            out.ok = true;
            return out;
        }
        catch (const std::exception & ex)
        {
            out.error = ex.what();
            return out;
        }
    }

private:
    template <typename Dur>
    static int64_t us(Dur d)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
    }

    neon::catalog::SchemaManager schema_;
    neon::storage::StorageEngine engine_;
    neon::planner::Planner planner_;
    neon::bytecode::Compiler compiler_;
    neon::bytecode::VirtualMachine vm_;
};

} // namespace neon::test
