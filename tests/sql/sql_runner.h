#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "src/base/error.h"
#include "src/bytecode/compiler.h"
#include "src/bytecode/fast_path.h"
#include "src/bytecode/txn_session.h"
#include "src/bytecode/vm.h"
#include "src/catalog/schema_mgr.h"
#include "src/executor/executor.h"
#include "src/parser/parser.h"
#include "src/planner/planner.h"
#include "src/storage/storage_engine.h"

namespace popo::test
{

struct SqlSuccess
{
    popo::bytecode::ExecuteSuccess result;
    popo::executor::Executor::SpillStats spill;
    int64_t parse_us = 0;
    int64_t plan_us = 0;
    int64_t compile_us = 0;
    int64_t vm_us = 0;
};

using SqlResult = popo::base::Result<SqlSuccess>;

class SqlRunner;

// Several SQL statements, one commit. Destructor aborts if commit was not called.
class SqlTxn
{
public:
    SqlTxn() = default;
    SqlTxn(SqlTxn && other) noexcept : runner_(other.runner_) { other.runner_ = nullptr; }
    SqlTxn & operator=(SqlTxn &&) = delete;
    SqlTxn(const SqlTxn &) = delete;
    SqlTxn & operator=(const SqlTxn &) = delete;
    ~SqlTxn();

    SqlResult exec(const std::string & sql);
    popo::base::Result<void> commit();

private:
    friend class SqlRunner;
    explicit SqlTxn(SqlRunner * runner);

    SqlRunner * runner_ = nullptr;
};

// Same pipeline as Session::handle_query: one SQL string, one statement at a time.
class SqlRunner
{
public:
    explicit SqlRunner(
        const std::string & data_dir,
        size_t buffer_pages = 4096,
        std::optional<popo::executor::Executor::Options> executor_options = std::nullopt)
        : engine_(data_dir, buffer_pages)
        , session_(engine_)
        , planner_(schema_)
        , compiler_(schema_)
        , vm_(schema_, engine_)
        , executor_options_(std::move(executor_options))
    {
        auto initialized = engine_.init();
        if (!initialized)
        {
            init_error_ = initialized.error();
        }
        else
        {
            schema_.restore(engine_.catalog());
        }
        vm_.set_session(&session_);
    }

    SqlResult exec(const std::string & sql) { return run(sql, nullptr); }

    // Run one statement inside a caller-owned transaction: a second "connection"
    // against the same engine, which is how the isolation tests interleave.
    SqlResult exec_in(const std::string & sql, popo::txn::Transaction * txn) { return run(sql, txn); }

    popo::base::Result<SqlTxn> begin()
    {
        if (init_error_)
        {
            return std::unexpected(*init_error_);
        }
        auto begun = session_.begin();
        if (!begun)
        {
            return std::unexpected(begun.error());
        }
        return SqlTxn(this);
    }

    // Isolation level for subsequent transactions.
    popo::bytecode::TxnSession & session() { return session_; }
    popo::storage::StorageEngine & engine() { return engine_; }
    popo::catalog::SchemaManager & schema() { return schema_; }
    void set_executor_options(popo::executor::Executor::Options options) { executor_options_ = std::move(options); }

private:
    friend class SqlTxn;

    SqlResult run(const std::string & sql, popo::txn::Transaction * txn)
    {
        using clock = std::chrono::steady_clock;
        if (init_error_)
        {
            return std::unexpected(*init_error_);
        }
        SqlSuccess out;
        // A caller-owned transaction belongs to the caller: never commit or roll
        // it back from here.
        const bool own_session = txn == nullptr;
        auto fail = [&](popo::base::Error error) -> SqlResult
        {
            if (own_session)
            {
                session_.statement_failed();
            }
            return std::unexpected(std::move(error));
        };

        auto * read_txn = own_session ? (session_.in_transaction() ? session_.current() : nullptr) : txn;
        if (auto fast = popo::bytecode::try_point_select(schema_, engine_, sql, read_txn))
        {
            out.result = std::move(*fast);
            return out;
        }
        auto t0 = clock::now();
        popo::parser::Parser parser;
        auto parsed = parser.parse(sql);
        auto t1 = clock::now();
        out.parse_us = us(t1 - t0);
        if (!parsed)
        {
            return fail(parsed.error());
        }

        for (const auto & stmt : *parsed)
        {
            auto tp0 = clock::now();
            auto plan = planner_.plan(stmt);
            auto tp1 = clock::now();
            out.plan_us += us(tp1 - tp0);
            if (!plan)
            {
                return fail(plan.error());
            }

            popo::txn::Transaction * use_txn = txn;
            if (own_session)
            {
                auto statement_txn = session_.statement_txn();
                if (!statement_txn)
                {
                    return fail(statement_txn.error());
                }
                use_txn = *statement_txn;
            }

            if (popo::executor::needs_generic_executor(stmt) || popo::executor::plan_needs_generic_executor(*plan))
            {
                std::unique_ptr<popo::executor::Executor> executor;
                if (executor_options_)
                    executor = std::make_unique<popo::executor::Executor>(schema_, engine_, *executor_options_);
                else
                    executor = std::make_unique<popo::executor::Executor>(schema_, engine_);
                auto executed = executor->execute(*plan, use_txn);
                auto tv1 = clock::now();
                out.vm_us += us(tv1 - tp1);
                if (!executed)
                {
                    return fail(executed.error());
                }
                out.spill = executor->spill_stats();
                out.result = std::move(*executed);
                if (own_session)
                {
                    auto succeeded = session_.statement_succeeded();
                    if (!succeeded)
                    {
                        return fail(succeeded.error());
                    }
                }
                continue;
            }

            auto compiled = compiler_.compile(*plan);
            auto tc1 = clock::now();
            out.compile_us += us(tc1 - tp1);
            if (!compiled)
            {
                return fail(compiled.error());
            }

            auto executed = vm_.execute(*compiled, use_txn);
            auto tv1 = clock::now();
            out.vm_us += us(tv1 - tc1);
            if (!executed)
            {
                return fail(executed.error());
            }
            out.result = std::move(*executed);
            if (own_session)
            {
                auto succeeded = session_.statement_succeeded();
                if (!succeeded)
                {
                    return fail(succeeded.error());
                }
            }
        }
        return out;
    }

    template <typename Dur>
    static int64_t us(Dur d)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
    }

    popo::catalog::SchemaManager schema_;
    popo::storage::StorageEngine engine_;
    popo::bytecode::TxnSession session_;
    popo::planner::Planner planner_;
    popo::bytecode::Compiler compiler_;
    popo::bytecode::VirtualMachine vm_;
    std::optional<popo::executor::Executor::Options> executor_options_;
    std::optional<popo::base::Error> init_error_;
};

inline SqlTxn::SqlTxn(SqlRunner * runner) : runner_(runner)
{
}

inline SqlTxn::~SqlTxn()
{
    if (runner_ != nullptr)
    {
        runner_->session_.rollback();
        runner_ = nullptr;
    }
}

inline SqlResult SqlTxn::exec(const std::string & sql)
{
    if (runner_ == nullptr)
    {
        return popo::base::make_error(popo::base::Errno::InvalidArg, "transaction is closed");
    }
    SqlResult out = runner_->run(sql, nullptr);
    if (!out)
    {
        runner_->session_.rollback();
        runner_ = nullptr;
    }
    return out;
}

inline popo::base::Result<void> SqlTxn::commit()
{
    if (runner_ == nullptr)
    {
        return popo::base::make_error(popo::base::Errno::InvalidArg, "transaction is closed");
    }
    auto committed = runner_->session_.commit();
    runner_ = nullptr;
    return committed;
}

} // namespace popo::test
