#pragma once

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include "src/bytecode/compiler.h"
#include "src/bytecode/fast_path.h"
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

class SqlRunner;

// Several SQL statements, one commit. Destructor aborts if commit was not called.
class SqlTxn
{
public:
    SqlTxn() = default;
    SqlTxn(SqlTxn && other) noexcept
        : runner_(other.runner_)
        , txn_(other.txn_)
    {
        other.runner_ = nullptr;
        other.txn_ = nullptr;
    }
    SqlTxn & operator=(SqlTxn &&) = delete;
    SqlTxn(const SqlTxn &) = delete;
    SqlTxn & operator=(const SqlTxn &) = delete;
    ~SqlTxn();

    SqlResult exec(const std::string & sql);
    void commit();

private:
    friend class SqlRunner;
    explicit SqlTxn(SqlRunner * runner);

    SqlRunner * runner_ = nullptr;
    neon::txn::Transaction * txn_ = nullptr;
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

    SqlResult exec(const std::string & sql) { return run(sql, nullptr); }

    SqlTxn begin() { return SqlTxn(this); }

private:
    friend class SqlTxn;

    SqlResult run(const std::string & sql, neon::txn::Transaction * outer)
    {
        using clock = std::chrono::steady_clock;
        SqlResult out;
        try
        {
            if (auto fast = neon::bytecode::try_point_select(schema_, engine_, sql))
            {
                out.result = std::move(*fast);
                out.ok = true;
                return out;
            }
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

                auto executed = vm_.execute(*compiled, outer);
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

inline SqlTxn::SqlTxn(SqlRunner * runner)
    : runner_(runner)
    , txn_(runner->engine_.begin_txn())
{
}

inline SqlTxn::~SqlTxn()
{
    if (txn_ != nullptr && runner_ != nullptr)
    {
        runner_->engine_.abort_txn(txn_);
        txn_ = nullptr;
    }
}

inline SqlResult SqlTxn::exec(const std::string & sql)
{
    if (txn_ == nullptr || runner_ == nullptr)
    {
        SqlResult out;
        out.error = "transaction is closed";
        return out;
    }
    SqlResult out = runner_->run(sql, txn_);
    if (!out.ok)
    {
        runner_->engine_.abort_txn(txn_);
        txn_ = nullptr;
    }
    return out;
}

inline void SqlTxn::commit()
{
    if (txn_ == nullptr || runner_ == nullptr)
    {
        throw std::runtime_error("transaction is closed");
    }
    runner_->engine_.commit_txn(txn_);
    txn_ = nullptr;
}

} // namespace neon::test
