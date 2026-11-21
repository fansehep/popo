#include "executor/executor.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "executor/executor_internal.h"
#include "function/date_functions.h"
#include "function/fast_like.h"
#include "function/function_registry.h"
#include "function/value_functions.h"

namespace popo::executor
{

using namespace popo::executor::detail;
using popo::function::add_date_interval;
using popo::function::as_bool;
using popo::function::cast_i64;
using popo::function::cast_to;
using popo::function::comparable;
using popo::function::do_arithmetic;
using popo::function::eq_ci;
using popo::function::FastLike;
using popo::function::FunctionRegistry;
using popo::function::is_floating;
using popo::function::is_null;
using popo::function::like_match;
using popo::function::make_bool;
using popo::function::sql_cmp;
using popo::function::sql_eq;
using popo::function::sql_lt;
using popo::parser::Expr;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::Value;
using popo::type::ValueType;

popo::base::Result<Value>
Executor::resolve_column(const std::string & table, const std::string & column, const EvalEnv & env) const
{
    const EvalEnv * scope = &env;
    while (scope)
    {
        if (scope->layout && scope->row)
        {
            size_t offset = 0;
            for (const auto & s : scope->layout->schemas)
            {
                const bool table_match = table.empty() || eq_ci(table, s.table)
                    || (!s.alias.empty() && eq_ci(table, s.alias));
                if (table_match)
                {
                    for (size_t i = 0; i < s.columns.size(); ++i)
                    {
                        if (eq_ci(s.columns[i], column))
                        {
                            min_env_depth_seen_ = std::min(min_env_depth_seen_, scope->depth);
                            const size_t idx = offset + i;
                            if (idx < scope->row->values_.size())
                            {
                                if (correlation_capture_ && scope->depth <= correlation_capture_max_depth_)
                                {
                                    bool captured = false;
                                    for (const auto & [seen_table, seen_column, _] : *correlation_capture_)
                                    {
                                        if (seen_table == table && seen_column == column)
                                        {
                                            captured = true;
                                            break;
                                        }
                                    }
                                    if (!captured)
                                        correlation_capture_->emplace_back(
                                            table, column, scope->row->values_[idx]);
                                }
                                return scope->row->values_[idx];
                            }
                            return Value();
                        }
                    }
                }
                offset += s.columns.size();
            }
        }
        scope = scope->outer;
    }
    return executor_error(std::format("Column '{}' not found", table.empty() ? column : table + "." + column));
}

popo::base::Result<Value>
Executor::eval_quantified(const popo::parser::QuantifiedExpr & expr, const EvalEnv & env)
{
    POPO_ASSIGN_OR_RETURN(Value operand, eval(expr.operand, env));
    Batch scratch;
    POPO_ASSIGN_OR_RETURN(auto * cached, eval_subquery_cached(expr, expr.query, env, scratch));
    const Batch & batch = cached ? cached->batch : scratch;
    if (batch.rows.empty())
        return make_bool(expr.all);
    if (expr.all)
    {
        for (const auto & row : batch.rows)
        {
            if (row.values_.empty() || !sql_cmp(expr.op, operand, row.values_[0]))
                return make_bool(false);
        }
        return make_bool(true);
    }
    for (const auto & row : batch.rows)
    {
        if (!row.values_.empty() && sql_cmp(expr.op, operand, row.values_[0]))
            return make_bool(true);
    }
    return make_bool(false);
}

popo::base::Result<Value>
Executor::eval_scalar(const popo::parser::ScalarCallExpr & expr, const EvalEnv & env)
{
    std::vector<Value> args;
    args.reserve(expr.args.size());
    for (const auto & arg : expr.args)
    {
        POPO_ASSIGN_OR_RETURN(auto value, eval(arg, env));
        args.push_back(std::move(value));
    }
    return FunctionRegistry::instance().call(expr.name, args);
}

popo::base::Result<Value> Executor::eval(const ExprPtr & expr, const EvalEnv & env)
{
    if (!expr)
        return Value();
    switch (expr->type)
    {
        case ExprType::Literal:
            return static_cast<const popo::parser::LiteralExpr &>(*expr).value;
        case ExprType::ColumnRef: {
            const auto & col = static_cast<const popo::parser::ColumnRefExpr &>(*expr);
            return resolve_column(col.table_name, col.column_name, env);
        }
        case ExprType::Alias:
            return eval(static_cast<const popo::parser::AliasExpr &>(*expr).expr, env);
        case ExprType::BinaryOp: {
            const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            if (bin.op == popo::parser::BinaryOpType::And)
            {
                POPO_ASSIGN_OR_RETURN(auto left, eval(bin.left, env));
                if (!as_bool(left))
                    return make_bool(false);
                POPO_ASSIGN_OR_RETURN(auto right, eval(bin.right, env));
                return make_bool(as_bool(right));
            }
            if (bin.op == popo::parser::BinaryOpType::Or)
            {
                POPO_ASSIGN_OR_RETURN(auto left, eval(bin.left, env));
                if (as_bool(left))
                    return make_bool(true);
                POPO_ASSIGN_OR_RETURN(auto right, eval(bin.right, env));
                return make_bool(as_bool(right));
            }
            if ((bin.op == popo::parser::BinaryOpType::Add || bin.op == popo::parser::BinaryOpType::Sub)
                && bin.right && bin.right->type == ExprType::Interval)
            {
                POPO_ASSIGN_OR_RETURN(Value dval, eval(bin.left, env));
                const auto & iv = static_cast<const popo::parser::IntervalExpr &>(*bin.right);
                POPO_ASSIGN_OR_RETURN(Value aval, eval(iv.value, env));
                if (is_null(dval) || is_null(aval))
                    return Value();
                POPO_ASSIGN_OR_RETURN(int64_t delta, cast_i64(aval));
                if (bin.op == popo::parser::BinaryOpType::Sub)
                    delta = -delta;
                std::string dstr = dval.datatype() == ValueType::String ? dval.string_ref() : dval.to_val_string();
                POPO_ASSIGN_OR_RETURN(auto date, add_date_interval(dstr, delta, iv.unit));
                return Value(ValueType::String, date);
            }
            if (bin.op == popo::parser::BinaryOpType::Add && bin.left && bin.left->type == ExprType::Interval)
            {
                const auto & iv = static_cast<const popo::parser::IntervalExpr &>(*bin.left);
                POPO_ASSIGN_OR_RETURN(Value aval, eval(iv.value, env));
                POPO_ASSIGN_OR_RETURN(Value dval, eval(bin.right, env));
                if (is_null(dval) || is_null(aval))
                    return Value();
                POPO_ASSIGN_OR_RETURN(int64_t delta, cast_i64(aval));
                std::string dstr = dval.datatype() == ValueType::String ? dval.string_ref() : dval.to_val_string();
                POPO_ASSIGN_OR_RETURN(auto date, add_date_interval(dstr, delta, iv.unit));
                return Value(ValueType::String, date);
            }
            POPO_ASSIGN_OR_RETURN(Value l, eval(bin.left, env));
            POPO_ASSIGN_OR_RETURN(Value r, eval(bin.right, env));
            switch (bin.op)
            {
                case popo::parser::BinaryOpType::Add:
                    return do_arithmetic('+', l, r);
                case popo::parser::BinaryOpType::Sub:
                    return do_arithmetic('-', l, r);
                case popo::parser::BinaryOpType::Mul:
                    return do_arithmetic('*', l, r);
                case popo::parser::BinaryOpType::Div:
                    return do_arithmetic('/', l, r);
                case popo::parser::BinaryOpType::Eq:
                    return make_bool(sql_eq(l, r));
                case popo::parser::BinaryOpType::Ne:
                    return make_bool(comparable(l, r) && !(l == r));
                case popo::parser::BinaryOpType::Lt:
                    return make_bool(sql_lt(l, r));
                case popo::parser::BinaryOpType::Le:
                    return make_bool(comparable(l, r) && !(r < l));
                case popo::parser::BinaryOpType::Gt:
                    return make_bool(sql_lt(r, l));
                case popo::parser::BinaryOpType::Ge:
                    return make_bool(comparable(l, r) && !(l < r));
                case popo::parser::BinaryOpType::Concat: {
                    if (is_null(l) || is_null(r))
                        return Value();
                    auto text = [](const Value & v)
                    { return v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string(); };
                    return Value(ValueType::String, text(l) + text(r));
                }
                default:
                    break;
            }
            return executor_error("unknown binary operator", popo::base::Errno::InternalErr);
        }
        case ExprType::UnaryOp: {
            const auto & un = static_cast<const popo::parser::UnaryOpExpr &>(*expr);
            POPO_ASSIGN_OR_RETURN(Value v, eval(un.operand, env));
            if (un.op == popo::parser::UnaryOpType::Not)
                return make_bool(!as_bool(v));
            if (is_null(v))
                return Value();
            if (is_floating(v))
                return Value(ValueType::Float64, -v.to_f64());
            return Value(ValueType::Int64, -static_cast<int64_t>(v.to_f64()));
        }
        case ExprType::FunctionCall: {
            const Expr * key = expr.get();
            const EvalEnv * scope = &env;
            while (scope)
            {
                if (scope->aggs)
                {
                    auto it = scope->aggs->find(key);
                    if (it != scope->aggs->end())
                    {
                        min_env_depth_seen_ = std::min(min_env_depth_seen_, scope->depth);
                        return it->second;
                    }
                }
                scope = scope->outer;
            }
            return executor_error("aggregate function not allowed in this context");
        }
        case ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
            if (in.list.empty())
                return make_bool(in.negated);
            POPO_ASSIGN_OR_RETURN(Value operand, eval(in.operand, env));
            if (is_null(operand))
                return make_bool(false);
            bool found = false;
            bool saw_null = false;
            for (const auto & item : in.list)
            {
                POPO_ASSIGN_OR_RETURN(Value iv, eval(item, env));
                if (is_null(iv))
                {
                    saw_null = true;
                    continue;
                }
                if (sql_eq(operand, iv))
                {
                    found = true;
                    break;
                }
            }
            if (found)
                return make_bool(!in.negated);
            if (in.negated && saw_null)
                return make_bool(false);
            return make_bool(in.negated);
        }
        case ExprType::Between: {
            const auto & b = static_cast<const popo::parser::BetweenExpr &>(*expr);
            POPO_ASSIGN_OR_RETURN(Value v, eval(b.operand, env));
            POPO_ASSIGN_OR_RETURN(Value lo, eval(b.low, env));
            POPO_ASSIGN_OR_RETURN(Value hi, eval(b.high, env));
            if (is_null(v) || is_null(lo) || is_null(hi))
                return make_bool(false);
            const bool in_range = comparable(v, lo) && comparable(v, hi) && !(v < lo) && !(hi < v);
            return make_bool(b.negated ? !in_range : in_range);
        }
        case ExprType::Like: {
            const auto & like = static_cast<const popo::parser::LikeExpr &>(*expr);
            POPO_ASSIGN_OR_RETURN(Value v, eval(like.operand, env));
            if (is_null(v))
                return make_bool(false);
            if (like.pattern && like.pattern->type == ExprType::Literal)
            {
                const auto & pat_val = static_cast<const popo::parser::LiteralExpr &>(*like.pattern).value;
                if (is_null(pat_val))
                    return make_bool(false);
                auto it = compiled_likes_.find(expr.get());
                if (it == compiled_likes_.end())
                {
                    const std::string pat_str = pat_val.datatype() == ValueType::String
                        ? pat_val.string_ref() : pat_val.to_val_string();
                    it = compiled_likes_.emplace(expr.get(), FastLike::compile(pat_str)).first;
                }
                const bool matched = (v.datatype() == ValueType::String)
                    ? it->second.match(v.string_ref())
                    : it->second.match(v.to_val_string());
                return make_bool(like.negated ? !matched : matched);
            }
            POPO_ASSIGN_OR_RETURN(Value p, eval(like.pattern, env));
            if (is_null(p))
                return make_bool(false);
            const bool matched = like_match(v.to_val_string(), p.to_val_string());
            return make_bool(like.negated ? !matched : matched);
        }
        case ExprType::IsNull: {
            const auto & isnull = static_cast<const popo::parser::IsNullExpr &>(*expr);
            POPO_ASSIGN_OR_RETURN(auto operand, eval(isnull.operand, env));
            const bool null = is_null(operand);
            return make_bool(isnull.negated ? !null : null);
        }
        case ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            Value operand;
            if (c.operand)
            {
                POPO_ASSIGN_OR_RETURN(operand, eval(c.operand, env));
            }
            for (const auto & [cond, result] : c.whens)
            {
                POPO_ASSIGN_OR_RETURN(Value cv, eval(cond, env));
                const bool hit = c.operand ? sql_eq(operand, cv) : as_bool(cv);
                if (hit)
                    return eval(result, env);
            }
            if (c.else_expr)
                return eval(c.else_expr, env);
            return Value();
        }
        case ExprType::Subquery:
        case ExprType::InSubquery:
        case ExprType::Exists:
            return eval_subquery(*expr, env);
        case ExprType::DistinctFrom: {
            const auto & d = static_cast<const popo::parser::DistinctFromExpr &>(*expr);
            POPO_ASSIGN_OR_RETURN(Value l, eval(d.left, env));
            POPO_ASSIGN_OR_RETURN(Value r, eval(d.right, env));
            const bool same = (is_null(l) && is_null(r)) || sql_eq(l, r);
            return make_bool(d.negated ? same : !same);
        }
        case ExprType::Quantified:
            return eval_quantified(static_cast<const popo::parser::QuantifiedExpr &>(*expr), env);
        case ExprType::ScalarCall:
            return eval_scalar(static_cast<const popo::parser::ScalarCallExpr &>(*expr), env);
        case ExprType::Cast: {
            const auto & cast_expr = static_cast<const popo::parser::CastExpr &>(*expr);
            POPO_ASSIGN_OR_RETURN(auto operand, eval(cast_expr.operand, env));
            return cast_to(operand, cast_expr.target);
        }
        case ExprType::Window: {
            if (!env.aggs)
                return executor_error("window function is not available here");
            auto found = env.aggs->find(expr.get());
            if (found == env.aggs->end())
                return executor_error("window function was not computed", popo::base::Errno::InternalErr);
            min_env_depth_seen_ = std::min(min_env_depth_seen_, env.depth);
            return found->second;
        }
        case ExprType::Interval:
            return executor_error("INTERVAL must be used with date + or -");
        case ExprType::Star:
            return executor_error("'*' is only valid in a select list or COUNT(*)");
    }
    return executor_error("unknown expression type", popo::base::Errno::InternalErr);
}

} // namespace popo::executor
