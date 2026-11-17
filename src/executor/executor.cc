#include "executor/executor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <set>
#include <string>

namespace popo::executor
{

using popo::parser::Expr;
using popo::parser::ExprPtr;
using popo::parser::ExprType;
using popo::type::Row;
using popo::type::Value;
using popo::type::ValueType;

#define POPO_EXEC_CONCAT_INNER(a, b) a##b
#define POPO_EXEC_CONCAT(a, b) POPO_EXEC_CONCAT_INNER(a, b)
#define POPO_ASSIGN_OR_RETURN_IMPL(lhs, expression, n)                         \
    auto POPO_EXEC_CONCAT(_popo_result_, n) = (expression);                    \
    if (!POPO_EXEC_CONCAT(_popo_result_, n))                                   \
        return std::unexpected(POPO_EXEC_CONCAT(_popo_result_, n).error());    \
    lhs = std::move(*POPO_EXEC_CONCAT(_popo_result_, n))
#define POPO_ASSIGN_OR_RETURN(lhs, expression) POPO_ASSIGN_OR_RETURN_IMPL(lhs, expression, __COUNTER__)

namespace
{

std::unexpected<popo::base::Error> executor_error(
    std::string message, popo::base::Errno code = popo::base::Errno::InvalidArg)
{
    return popo::base::make_error(code, std::move(message));
}

Executor::Options default_executor_options()
{
    Executor::Options options;
    if (const char * raw = std::getenv("POPO_EXECUTOR_MEMORY_BYTES"))
    {
        auto parsed = popo::base::parse_i64(raw);
        if (parsed && *parsed >= 4096)
            options.memory_limit_bytes = static_cast<size_t>(*parsed);
    }
    if (const char * directory = std::getenv("POPO_EXECUTOR_TEMP_DIR"))
        options.temp_directory = directory;
    return options;
}

bool is_null(const Value & v)
{
    return v.datatype() == ValueType::Null;
}

bool as_bool(const Value & v)
{
    switch (v.datatype())
    {
        case ValueType::Bool:
            return v.boolean();
        case ValueType::Null:
            return false;
        case ValueType::String:
            return !v.string_ref().empty();
        default:
            return v.is_numeric() && v.to_f64() != 0.0;
    }
}

bool is_floating(const Value & v)
{
    return v.datatype() == ValueType::Float32 || v.datatype() == ValueType::Float64;
}

int64_t to_i64(const Value & v)
{
    switch (v.datatype())
    {
        case ValueType::Int8:
            return v.i8();
        case ValueType::Int16:
            return v.i16();
        case ValueType::Int32:
            return v.i32();
        case ValueType::Int64:
            return v.i64();
        case ValueType::Uint8:
            return static_cast<int64_t>(v.u8());
        case ValueType::Uint16:
            return static_cast<int64_t>(v.u16());
        case ValueType::Uint32:
            return static_cast<int64_t>(v.u32());
        case ValueType::Uint64:
            return static_cast<int64_t>(v.u64());
        case ValueType::Bool:
            return v.boolean() ? 1 : 0;
        default:
            return static_cast<int64_t>(v.to_f64());
    }
}

Value make_bool(bool b)
{
    return Value(ValueType::Bool, b);
}

// SQL comparison semantics: NULL or incompatible types yield unknown, treated as false.
bool comparable(const Value & l, const Value & r)
{
    return !is_null(l) && !is_null(r) && (l.datatype() == r.datatype() || (l.is_numeric() && r.is_numeric()));
}

bool sql_eq(const Value & l, const Value & r)
{
    return comparable(l, r) && l == r;
}

bool sql_lt(const Value & l, const Value & r)
{
    return comparable(l, r) && l < r;
}

popo::base::Result<Value> do_arithmetic(char op, const Value & l, const Value & r)
{
    if (is_null(l) || is_null(r))
        return Value();
    bool floating = is_floating(l) || is_floating(r);
    switch (op)
    {
        case '+':
            return floating ? Value(ValueType::Float64, l.to_f64() + r.to_f64()) : Value(ValueType::Int64, to_i64(l) + to_i64(r));
        case '-':
            return floating ? Value(ValueType::Float64, l.to_f64() - r.to_f64()) : Value(ValueType::Int64, to_i64(l) - to_i64(r));
        case '*':
            return floating ? Value(ValueType::Float64, l.to_f64() * r.to_f64()) : Value(ValueType::Int64, to_i64(l) * to_i64(r));
        case '/':
            if (r.to_f64() == 0.0)
                return Value();
            if (floating)
                return Value(ValueType::Float64, l.to_f64() / r.to_f64());
            return Value(ValueType::Int64, to_i64(l) / to_i64(r));
        default:
            return executor_error("unknown arithmetic operator", popo::base::Errno::InternalErr);
    }
}

// SQL LIKE pattern matching: '%' matches any sequence, '_' matches one character.
bool like_match(const std::string & text, const std::string & pattern)
{
    size_t t = 0;
    size_t p = 0;
    size_t star = std::string::npos;
    size_t backtrack = 0;
    while (t < text.size())
    {
        if (p < pattern.size() && (pattern[p] == '_' || pattern[p] == text[t]))
        {
            ++t;
            ++p;
        }
        else if (p < pattern.size() && pattern[p] == '%')
        {
            star = p++;
            backtrack = t;
        }
        else if (star != std::string::npos)
        {
            p = star + 1;
            t = ++backtrack;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '%')
    {
        ++p;
    }
    return p == pattern.size();
}

bool eq_ci(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string lower_str(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool parse_ymd(const std::string & text, int & y, int & m, int & d)
{
    size_t p = 0;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p])))
        ++p;
    auto read_int = [&](int & out) -> bool
    {
        if (p >= text.size() || !std::isdigit(static_cast<unsigned char>(text[p])))
            return false;
        int v = 0;
        while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p])))
        {
            v = v * 10 + (text[p] - '0');
            ++p;
        }
        out = v;
        return true;
    };
    if (!read_int(y) || p >= text.size() || text[p] != '-')
        return false;
    ++p;
    if (!read_int(m) || p >= text.size() || text[p] != '-')
        return false;
    ++p;
    if (!read_int(d))
        return false;
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

bool is_leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int days_in_month(int y, int m)
{
    static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && is_leap_year(y))
        return 29;
    return (m >= 1 && m <= 12) ? kDays[m] : 30;
}

int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (static_cast<unsigned>(m) + (m > 2 ? static_cast<unsigned>(-3) : 9u)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t z, int & y, int & m, int & d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2 ? 1 : 0;
}

popo::base::Result<std::string>
add_date_interval(const std::string & raw, int64_t delta, popo::parser::IntervalUnit unit)
{
    int y = 0;
    int m = 0;
    int d = 0;
    if (!parse_ymd(raw, y, m, d))
    {
        return executor_error("invalid date '" + raw + "'");
    }
    switch (unit)
    {
        case popo::parser::IntervalUnit::Year:
            y = static_cast<int>(y + delta);
            d = std::min(d, days_in_month(y, m));
            break;
        case popo::parser::IntervalUnit::Month: {
            int64_t total = static_cast<int64_t>(y) * 12 + (m - 1) + delta;
            int64_t ny = total >= 0 ? total / 12 : (total - 11) / 12;
            int nm = static_cast<int>(total - ny * 12) + 1;
            y = static_cast<int>(ny);
            m = nm;
            d = std::min(d, days_in_month(y, m));
            break;
        }
        case popo::parser::IntervalUnit::Day: {
            int64_t days = days_from_civil(y, m, d) + delta;
            civil_from_days(days, y, m, d);
            break;
        }
    }
    return std::format("{:04d}-{:02d}-{:02d}", y, m, d);
}

std::string value_key(const Value & v)
{
    if (is_null(v))
        return "N";
    if (v.datatype() == ValueType::Bool)
        return v.boolean() ? "B:1" : "B:0";
    if (v.datatype() == ValueType::String)
        return "S:" + v.string_ref();
    if (v.datatype() == ValueType::Char)
        return std::string("S:") + static_cast<char>(v.u8());
    if (v.is_numeric())
    {
        double d = v.to_f64();
        if (d == 0.0)
            d = 0.0;
        std::string out("D:", 2);
        out.append(reinterpret_cast<const char *>(&d), sizeof(double));
        return out;
    }
    return std::to_string(static_cast<int32_t>(v.datatype())) + ":" + v.to_val_string();
}

std::string row_key(const Row & row)
{
    std::string key;
    for (const auto & v : row.values_)
    {
        key += value_key(v);
        key += '\x1f';
    }
    return key;
}

std::string agg_display_name(const popo::parser::FunctionCallExpr & func)
{
    static const char * kNames[] = {"COUNT", "SUM", "AVG", "MIN", "MAX", "XMLAGG"};
    std::string name = kNames[static_cast<int32_t>(func.func)];
    name += "(";
    if (!func.args.empty() && func.args[0]->type == ExprType::ColumnRef)
    {
        name += static_cast<const popo::parser::ColumnRefExpr &>(*func.args[0]).column_name;
    }
    else if (!func.args.empty() && func.args[0]->type != ExprType::Star)
    {
        name += "...";
    }
    else
    {
        name += "*";
    }
    name += ")";
    return name;
}

bool expr_has_new_syntax(const ExprPtr & expr);

bool stmt_has_new_syntax(const popo::parser::StmtPtr & stmt)
{
    if (!stmt)
        return false;
    if (stmt->type == popo::parser::StmtType::AlterTable || stmt->type == popo::parser::StmtType::CreateView
        || stmt->type == popo::parser::StmtType::DropView)
        return true;
    if (stmt->type == popo::parser::StmtType::Select)
    {
        const auto & sel = static_cast<const popo::parser::SelectStatement &>(*stmt);
        if (sel.distinct || sel.offset.has_value() || !sel.set_ops.empty() || !sel.with_clause.empty() || !sel.from
            || !sel.group_by.empty())
            return true;
        bool has_any_agg = sel.having != nullptr;
        for (const auto & e : sel.select_list)
        {
            if (e && e->type == ExprType::FunctionCall)
                has_any_agg = true;
        }
        for (const auto & cte : sel.with_clause)
        {
            if (stmt_has_new_syntax(cte.query))
                return true;
        }
        for (const auto & term : sel.set_ops)
        {
            if (stmt_has_new_syntax(term.query))
                return true;
        }
        std::function<bool(const popo::parser::TableRefPtr &)> check_ref = [&](const popo::parser::TableRefPtr & ref) -> bool
        {
            if (!ref)
                return false;
            if (ref->type == popo::parser::TableRefType::Derived)
            {
                return true;
            }
            if (ref->type == popo::parser::TableRefType::Simple)
            {
                const auto & s = static_cast<const popo::parser::SimpleTableRef &>(*ref);
                return !s.column_aliases.empty();
            }
            if (ref->type == popo::parser::TableRefType::Join)
            {
                const auto & join = static_cast<const popo::parser::JoinTableRef &>(*ref);
                // Bytecode VM only runs simple 2-table INNER JOIN without agg/sort/limit or self-join.
                if (join.join_type != popo::parser::JoinType::Inner || has_any_agg || !sel.order_by.empty() || sel.limit.has_value())
                    return true;
                if (!join.left || !join.right || join.left->type != popo::parser::TableRefType::Simple
                    || join.right->type != popo::parser::TableRefType::Simple)
                    return true;
                const auto & ls = static_cast<const popo::parser::SimpleTableRef &>(*join.left);
                const auto & rs = static_cast<const popo::parser::SimpleTableRef &>(*join.right);
                if (eq_ci(ls.table_name, rs.table_name))
                    return true;
                return check_ref(join.left) || check_ref(join.right) || expr_has_new_syntax(join.condition);
            }
            return false;
        };
        if (check_ref(sel.from))
            return true;
        for (const auto & e : sel.select_list)
        {
            if (expr_has_new_syntax(e))
                return true;
        }
        if (expr_has_new_syntax(sel.where) || expr_has_new_syntax(sel.having))
            return true;
        for (const auto & e : sel.group_by)
        {
            if (expr_has_new_syntax(e))
                return true;
        }
        for (const auto & item : sel.order_by)
        {
            if (expr_has_new_syntax(item.expr))
                return true;
        }
        return false;
    }
    if (stmt->type == popo::parser::StmtType::Insert)
    {
        const auto & ins = static_cast<const popo::parser::InsertStatement &>(*stmt);
        for (const auto & row : ins.values)
        {
            for (const auto & e : row)
            {
                if (expr_has_new_syntax(e))
                    return true;
            }
        }
        return false;
    }
    if (stmt->type == popo::parser::StmtType::Update)
    {
        const auto & upd = static_cast<const popo::parser::UpdateStatement &>(*stmt);
        if (expr_has_new_syntax(upd.where))
            return true;
        for (const auto & assign : upd.assignments)
        {
            if (expr_has_new_syntax(assign.value))
                return true;
        }
        return false;
    }
    if (stmt->type == popo::parser::StmtType::Delete)
    {
        return expr_has_new_syntax(static_cast<const popo::parser::DeleteStatement &>(*stmt).where);
    }
    return false;
}

bool expr_has_new_syntax(const ExprPtr & expr)
{
    if (!expr)
        return false;
    switch (expr->type)
    {
        case ExprType::In:
        case ExprType::Between:
        case ExprType::Like:
        case ExprType::IsNull:
        case ExprType::Case:
        case ExprType::Subquery:
        case ExprType::InSubquery:
        case ExprType::Exists:
        case ExprType::Alias:
        case ExprType::DistinctFrom:
        case ExprType::Quantified:
        case ExprType::ScalarCall:
        case ExprType::Cast:
        case ExprType::Window:
        case ExprType::Interval:
            return true;
        case ExprType::BinaryOp: {
            const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            if (bin.op == popo::parser::BinaryOpType::Concat)
                return true;
            return expr_has_new_syntax(bin.left) || expr_has_new_syntax(bin.right);
        }
        case ExprType::UnaryOp:
            return expr_has_new_syntax(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand);
        case ExprType::FunctionCall: {
            const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*expr);
            if (func.is_distinct || func.func == popo::parser::AggFuncType::XmlAgg)
                return true;
            for (const auto & arg : func.args)
            {
                if (expr_has_new_syntax(arg))
                    return true;
            }
            return false;
        }
        default:
            return false;
    }
}

} // namespace

bool needs_generic_executor(const popo::parser::StmtPtr & stmt)
{
    return stmt_has_new_syntax(stmt);
}

bool plan_needs_generic_executor(const popo::planner::PlanNodePtr & node)
{
    if (!node)
        return false;
    using popo::planner::PlanNodeType;
    switch (node->type)
    {
        case PlanNodeType::AlterTable:
        case PlanNodeType::Distinct:
        case PlanNodeType::SetOp:
        case PlanNodeType::SubqueryScan:
        case PlanNodeType::RecursiveScan:
        case PlanNodeType::Window:
        case PlanNodeType::Dual:
        case PlanNodeType::CreateView:
        case PlanNodeType::DropView:
            return true;
        case PlanNodeType::Filter:
            return plan_needs_generic_executor(static_cast<const popo::planner::FilterNode &>(*node).child);
        case PlanNodeType::Project:
            return plan_needs_generic_executor(static_cast<const popo::planner::ProjectNode &>(*node).child);
        case PlanNodeType::Agg:
            return plan_needs_generic_executor(static_cast<const popo::planner::AggNode &>(*node).child);
        case PlanNodeType::Sort:
            return plan_needs_generic_executor(static_cast<const popo::planner::SortNode &>(*node).child);
        case PlanNodeType::Limit:
            return plan_needs_generic_executor(static_cast<const popo::planner::LimitNode &>(*node).child);
        case PlanNodeType::Join: {
            const auto & join = static_cast<const popo::planner::JoinNode &>(*node);
            return plan_needs_generic_executor(join.left) || plan_needs_generic_executor(join.right);
        }
        default:
            return false;
    }
}

namespace
{

void split_and_conjuncts(const ExprPtr & expr, std::vector<ExprPtr> & out)
{
    if (!expr)
        return;
    if (expr->type == ExprType::BinaryOp)
    {
        const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
        if (bin.op == popo::parser::BinaryOpType::And)
        {
            split_and_conjuncts(bin.left, out);
            split_and_conjuncts(bin.right, out);
            return;
        }
    }
    out.push_back(expr);
}

void split_or_branches(const ExprPtr & expr, std::vector<ExprPtr> & out)
{
    if (!expr)
        return;
    if (expr->type == ExprType::BinaryOp)
    {
        const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
        if (bin.op == popo::parser::BinaryOpType::Or)
        {
            split_or_branches(bin.left, out);
            split_or_branches(bin.right, out);
            return;
        }
    }
    out.push_back(expr);
}

bool same_col_ref(const ExprPtr & a, const ExprPtr & b)
{
    if (!a || !b || a->type != ExprType::ColumnRef || b->type != ExprType::ColumnRef)
        return false;
    const auto & ca = static_cast<const popo::parser::ColumnRefExpr &>(*a);
    const auto & cb = static_cast<const popo::parser::ColumnRefExpr &>(*b);
    return eq_ci(ca.table_name, cb.table_name) && eq_ci(ca.column_name, cb.column_name);
}

bool same_equijoin_expr(const ExprPtr & a, const ExprPtr & b)
{
    if (!a || !b || a->type != ExprType::BinaryOp || b->type != ExprType::BinaryOp)
        return false;
    const auto & ba = static_cast<const popo::parser::BinaryOpExpr &>(*a);
    const auto & bb = static_cast<const popo::parser::BinaryOpExpr &>(*b);
    if (ba.op != popo::parser::BinaryOpType::Eq || bb.op != popo::parser::BinaryOpType::Eq)
        return false;
    return (same_col_ref(ba.left, bb.left) && same_col_ref(ba.right, bb.right))
        || (same_col_ref(ba.left, bb.right) && same_col_ref(ba.right, bb.left));
}

// Extract equijoin predicates shared by every branch of an OR (such as TPC-H Q19).
void extract_common_or_equijoins(const ExprPtr & expr, std::vector<ExprPtr> & out)
{
    if (!expr || expr->type != ExprType::BinaryOp)
        return;
    const auto & bin = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
    if (bin.op != popo::parser::BinaryOpType::Or)
        return;
    std::vector<ExprPtr> branches;
    split_or_branches(expr, branches);
    if (branches.size() < 2)
        return;
    std::vector<std::vector<ExprPtr>> branch_conjuncts(branches.size());
    for (size_t i = 0; i < branches.size(); ++i)
    {
        split_and_conjuncts(branches[i], branch_conjuncts[i]);
    }
    for (const auto & cand : branch_conjuncts[0])
    {
        if (!cand || cand->type != ExprType::BinaryOp)
            continue;
        const auto & cb = static_cast<const popo::parser::BinaryOpExpr &>(*cand);
        if (cb.op != popo::parser::BinaryOpType::Eq || !cb.left || !cb.right
            || cb.left->type != ExprType::ColumnRef || cb.right->type != ExprType::ColumnRef)
            continue;
        bool in_all = true;
        for (size_t i = 1; i < branch_conjuncts.size(); ++i)
        {
            bool found = false;
            for (const auto & other : branch_conjuncts[i])
            {
                if (same_equijoin_expr(cand, other))
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                in_all = false;
                break;
            }
        }
        if (in_all)
        {
            out.push_back(cand);
        }
    }
}

ExprPtr combine_and(const std::vector<ExprPtr> & conjuncts)
{
    if (conjuncts.empty())
        return nullptr;
    ExprPtr res = conjuncts[0];
    for (size_t i = 1; i < conjuncts.size(); ++i)
    {
        auto b = std::make_shared<popo::parser::BinaryOpExpr>();
        b->op = popo::parser::BinaryOpType::And;
        b->left = res;
        b->right = conjuncts[i];
        res = b;
    }
    return res;
}

bool expr_has_subquery_or_agg(const ExprPtr & expr)
{
    if (!expr)
        return false;
    switch (expr->type)
    {
        case ExprType::Subquery:
        case ExprType::InSubquery:
        case ExprType::Exists:
        case ExprType::Quantified:
        case ExprType::FunctionCall:
        case ExprType::Window:
            return true;
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            return expr_has_subquery_or_agg(b.left) || expr_has_subquery_or_agg(b.right);
        }
        case ExprType::UnaryOp:
            return expr_has_subquery_or_agg(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand);
        case ExprType::Alias:
            return expr_has_subquery_or_agg(static_cast<const popo::parser::AliasExpr &>(*expr).expr);
        case ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
            if (expr_has_subquery_or_agg(in.operand))
                return true;
            for (const auto & item : in.list)
            {
                if (expr_has_subquery_or_agg(item))
                    return true;
            }
            return false;
        }
        case ExprType::Between: {
            const auto & b = static_cast<const popo::parser::BetweenExpr &>(*expr);
            return expr_has_subquery_or_agg(b.operand) || expr_has_subquery_or_agg(b.low) || expr_has_subquery_or_agg(b.high);
        }
        case ExprType::Like: {
            const auto & l = static_cast<const popo::parser::LikeExpr &>(*expr);
            return expr_has_subquery_or_agg(l.operand) || expr_has_subquery_or_agg(l.pattern);
        }
        case ExprType::IsNull:
            return expr_has_subquery_or_agg(static_cast<const popo::parser::IsNullExpr &>(*expr).operand);
        case ExprType::DistinctFrom: {
            const auto & d = static_cast<const popo::parser::DistinctFromExpr &>(*expr);
            return expr_has_subquery_or_agg(d.left) || expr_has_subquery_or_agg(d.right);
        }
        case ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            if (expr_has_subquery_or_agg(c.operand) || expr_has_subquery_or_agg(c.else_expr))
                return true;
            for (const auto & [w, t] : c.whens)
            {
                if (expr_has_subquery_or_agg(w) || expr_has_subquery_or_agg(t))
                    return true;
            }
            return false;
        }
        case ExprType::ScalarCall: {
            const auto & sc = static_cast<const popo::parser::ScalarCallExpr &>(*expr);
            for (const auto & a : sc.args)
            {
                if (expr_has_subquery_or_agg(a))
                    return true;
            }
            return false;
        }
        case ExprType::Cast:
            return expr_has_subquery_or_agg(static_cast<const popo::parser::CastExpr &>(*expr).operand);
        case ExprType::Interval:
            return expr_has_subquery_or_agg(static_cast<const popo::parser::IntervalExpr &>(*expr).value);
        default:
            return false;
    }
}

void collect_col_refs(const ExprPtr & expr, std::vector<const popo::parser::ColumnRefExpr *> & out)
{
    if (!expr)
        return;
    switch (expr->type)
    {
        case ExprType::ColumnRef:
            out.push_back(&static_cast<const popo::parser::ColumnRefExpr &>(*expr));
            return;
        case ExprType::BinaryOp: {
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*expr);
            collect_col_refs(b.left, out);
            collect_col_refs(b.right, out);
            return;
        }
        case ExprType::UnaryOp:
            collect_col_refs(static_cast<const popo::parser::UnaryOpExpr &>(*expr).operand, out);
            return;
        case ExprType::Alias:
            collect_col_refs(static_cast<const popo::parser::AliasExpr &>(*expr).expr, out);
            return;
        case ExprType::In: {
            const auto & in = static_cast<const popo::parser::InExpr &>(*expr);
            collect_col_refs(in.operand, out);
            for (const auto & item : in.list)
                collect_col_refs(item, out);
            return;
        }
        case ExprType::Between: {
            const auto & b = static_cast<const popo::parser::BetweenExpr &>(*expr);
            collect_col_refs(b.operand, out);
            collect_col_refs(b.low, out);
            collect_col_refs(b.high, out);
            return;
        }
        case ExprType::Like: {
            const auto & l = static_cast<const popo::parser::LikeExpr &>(*expr);
            collect_col_refs(l.operand, out);
            collect_col_refs(l.pattern, out);
            return;
        }
        case ExprType::IsNull:
            collect_col_refs(static_cast<const popo::parser::IsNullExpr &>(*expr).operand, out);
            return;
        case ExprType::DistinctFrom: {
            const auto & d = static_cast<const popo::parser::DistinctFromExpr &>(*expr);
            collect_col_refs(d.left, out);
            collect_col_refs(d.right, out);
            return;
        }
        case ExprType::Case: {
            const auto & c = static_cast<const popo::parser::CaseExpr &>(*expr);
            collect_col_refs(c.operand, out);
            for (const auto & [w, t] : c.whens)
            {
                collect_col_refs(w, out);
                collect_col_refs(t, out);
            }
            collect_col_refs(c.else_expr, out);
            return;
        }
        case ExprType::ScalarCall: {
            const auto & sc = static_cast<const popo::parser::ScalarCallExpr &>(*expr);
            for (const auto & a : sc.args)
                collect_col_refs(a, out);
            return;
        }
        case ExprType::Cast:
            collect_col_refs(static_cast<const popo::parser::CastExpr &>(*expr).operand, out);
            return;
        case ExprType::Interval:
            collect_col_refs(static_cast<const popo::parser::IntervalExpr &>(*expr).value, out);
            return;
        default:
            return;
    }
}

} // namespace

size_t Executor::Batch::width() const
{
    size_t n = 0;
    for (const auto & s : schemas)
    {
        n += s.columns.size();
    }
    return n;
}

std::vector<std::string> Executor::Batch::flat_names() const
{
    std::vector<std::string> names;
    for (const auto & s : schemas)
    {
        names.insert(names.end(), s.columns.begin(), s.columns.end());
    }
    return names;
}

Executor::Executor(popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine)
    : Executor(schema_mgr, engine, default_executor_options())
{
}

Executor::Executor(
    popo::catalog::SchemaManager & schema_mgr, popo::storage::StorageEngine & engine,
    Options options)
    : schema_mgr_(schema_mgr), engine_(engine), planner_(schema_mgr), options_(std::move(options))
{
}

bool Executor::collect_schemas(const popo::planner::PlanNodePtr & node, std::vector<ColumnSchema> & out)
{
    if (!node)
        return false;
    using popo::planner::PlanNodeType;
    switch (node->type)
    {
        case PlanNodeType::Scan: {
            const auto & scan = static_cast<const popo::planner::ScanNode &>(*node);
            ColumnSchema cs;
            cs.table = scan.alias.empty() ? scan.table_name : scan.alias;
            cs.alias = scan.alias;
            cs.columns = scan.column_names;
            out.push_back(std::move(cs));
            return true;
        }
        case PlanNodeType::SubqueryScan: {
            const auto & sub = static_cast<const popo::planner::SubqueryScanNode &>(*node);
            if (sub.column_names.empty())
                return false;
            ColumnSchema cs;
            cs.table = sub.alias;
            cs.alias = sub.alias;
            cs.columns = sub.column_names;
            out.push_back(std::move(cs));
            return true;
        }
        case PlanNodeType::Filter:
            return collect_schemas(static_cast<const popo::planner::FilterNode &>(*node).child, out);
        case PlanNodeType::Join: {
            const auto & join = static_cast<const popo::planner::JoinNode &>(*node);
            return collect_schemas(join.left, out) && collect_schemas(join.right, out);
        }
        default:
            return false;
    }
}

popo::planner::PlanNodePtr
Executor::optimize_join_group(const popo::planner::PlanNodePtr & root, bool preserve_star_order) const
{
    using popo::planner::FilterNode;
    using popo::planner::JoinNode;
    using popo::planner::PlanNodePtr;
    using popo::planner::PlanNodeType;

    std::vector<PlanNodePtr> leaves;
    std::vector<ExprPtr> conjuncts;

    auto gather = [&](const PlanNodePtr & cur, auto & self) -> void
    {
        if (!cur)
            return;
        if (cur->type == PlanNodeType::Filter)
        {
            const auto & f = static_cast<const FilterNode &>(*cur);
            split_and_conjuncts(f.condition, conjuncts);
            self(f.child, self);
            return;
        }
        if (cur->type == PlanNodeType::Join)
        {
            const auto & j = static_cast<const JoinNode &>(*cur);
            if (j.join_type == popo::parser::JoinType::Inner || j.join_type == popo::parser::JoinType::Cross)
            {
                if (j.condition)
                    split_and_conjuncts(j.condition, conjuncts);
                self(j.left, self);
                self(j.right, self);
                return;
            }
        }
        leaves.push_back(optimize_plan(cur, false));
    };
    gather(root, gather);

    if (leaves.size() <= 1)
    {
        if (leaves.empty())
            return root;
        PlanNodePtr out = leaves[0];
        if (!conjuncts.empty())
        {
            auto f = std::make_shared<FilterNode>();
            f->child = out;
            f->condition = combine_and(conjuncts);
            out = f;
        }
        return out;
    }

    // Also surface equijoins shared across every branch of an OR predicate (TPC-H Q19).
    std::vector<ExprPtr> extra_equijoins;
    for (const auto & c : conjuncts)
    {
        extract_common_or_equijoins(c, extra_equijoins);
    }
    conjuncts.insert(conjuncts.end(), extra_equijoins.begin(), extra_equijoins.end());

    const size_t m = leaves.size();
    std::vector<std::vector<ColumnSchema>> leaf_schemas(m);
    for (size_t i = 0; i < m; ++i)
    {
        if (!collect_schemas(leaves[i], leaf_schemas[i]))
        {
            // Fallback: keep structure, only optimize children.
            if (root->type == PlanNodeType::Filter)
            {
                const auto & f = static_cast<const FilterNode &>(*root);
                auto copy = std::make_shared<FilterNode>();
                copy->child = optimize_plan(f.child, preserve_star_order);
                copy->condition = f.condition;
                return copy;
            }
            const auto & j = static_cast<const JoinNode &>(*root);
            auto copy = std::make_shared<JoinNode>();
            copy->join_type = j.join_type;
            copy->left = optimize_plan(j.left, preserve_star_order);
            copy->right = optimize_plan(j.right, preserve_star_order);
            copy->condition = j.condition;
            return copy;
        }
    }

    auto schema_has_col = [](const ColumnSchema & s, const std::string & table, const std::string & col) -> bool
    {
        bool tmatch = table.empty() || eq_ci(table, s.table) || (!s.alias.empty() && eq_ci(table, s.alias));
        if (!tmatch)
            return false;
        for (const auto & c : s.columns)
        {
            if (eq_ci(c, col))
                return true;
        }
        return false;
    };

    auto expr_leaf_deps = [&](const ExprPtr & expr, std::set<size_t> & deps, bool & ambiguous)
    {
        deps.clear();
        ambiguous = false;
        std::vector<const popo::parser::ColumnRefExpr *> refs;
        collect_col_refs(expr, refs);
        for (const auto * ref : refs)
        {
            size_t hits = 0;
            size_t hit_leaf = 0;
            for (size_t i = 0; i < m; ++i)
            {
                bool in_leaf = false;
                for (const auto & s : leaf_schemas[i])
                {
                    if (schema_has_col(s, ref->table_name, ref->column_name))
                    {
                        in_leaf = true;
                        break;
                    }
                }
                if (in_leaf)
                {
                    ++hits;
                    hit_leaf = i;
                }
            }
            if (hits > 1)
            {
                ambiguous = true;
                return;
            }
            if (hits == 1)
            {
                deps.insert(hit_leaf);
            }
        }
    };

    struct JoinPred
    {
        ExprPtr expr;
        std::set<size_t> deps;
        bool is_equi = false;
        bool placed = false;
    };

    std::vector<std::vector<ExprPtr>> leaf_filters(m);
    std::vector<ExprPtr> const_filters;
    std::vector<JoinPred> join_preds;
    std::vector<ExprPtr> post_filters;

    for (const auto & c : conjuncts)
    {
        if (expr_has_subquery_or_agg(c))
        {
            post_filters.push_back(c);
            continue;
        }
        std::set<size_t> deps;
        bool ambiguous = false;
        expr_leaf_deps(c, deps, ambiguous);
        if (ambiguous)
        {
            post_filters.push_back(c);
            continue;
        }
        if (deps.empty())
        {
            const_filters.push_back(c);
        }
        else if (deps.size() == 1)
        {
            leaf_filters[*deps.begin()].push_back(c);
        }
        else
        {
            bool is_equi = false;
            if (c->type == ExprType::BinaryOp)
            {
                const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*c);
                if (b.op == popo::parser::BinaryOpType::Eq && deps.size() == 2)
                {
                    std::set<size_t> ld, rd;
                    bool la = false, ra = false;
                    expr_leaf_deps(b.left, ld, la);
                    expr_leaf_deps(b.right, rd, ra);
                    if (!la && !ra && ld.size() == 1 && rd.size() == 1 && *ld.begin() != *rd.begin())
                        is_equi = true;
                }
            }
            join_preds.push_back(JoinPred{c, std::move(deps), is_equi, false});
        }
    }

    std::vector<bool> has_pushed_filter(m, false);
    for (size_t i = 0; i < m; ++i)
    {
        if (i == 0 && !const_filters.empty())
        {
            leaf_filters[0].insert(leaf_filters[0].begin(), const_filters.begin(), const_filters.end());
        }
        if (!leaf_filters[i].empty())
        {
            has_pushed_filter[i] = true;
            auto f = std::make_shared<FilterNode>();
            f->child = leaves[i];
            f->condition = combine_and(leaf_filters[i]);
            leaves[i] = f;
        }
    }

    std::vector<size_t> order;
    order.reserve(m);
    if (preserve_star_order)
    {
        for (size_t i = 0; i < m; ++i)
            order.push_back(i);
    }
    else
    {
        std::vector<bool> used(m, false);
        size_t first = 0;
        for (size_t i = 0; i < m; ++i)
        {
            if (has_pushed_filter[i])
            {
                first = i;
                break;
            }
        }
        used[first] = true;
        order.push_back(first);

        auto in_joined = [&](size_t idx) { return used[idx]; };
        while (order.size() < m)
        {
            size_t best = m;
            int best_rank = -1;
            for (size_t cand = 0; cand < m; ++cand)
            {
                if (used[cand])
                    continue;
                bool has_equi = false;
                bool has_any = false;
                for (const auto & jp : join_preds)
                {
                    if (!jp.deps.contains(cand))
                        continue;
                    bool all_ready = true;
                    for (size_t d : jp.deps)
                    {
                        if (d != cand && !in_joined(d))
                        {
                            all_ready = false;
                            break;
                        }
                    }
                    if (all_ready)
                    {
                        has_any = true;
                        if (jp.is_equi)
                            has_equi = true;
                    }
                }
                int rank = 0;
                if (has_equi)
                    rank = has_pushed_filter[cand] ? 4 : 3;
                else if (has_any)
                    rank = has_pushed_filter[cand] ? 2 : 1;
                if (rank > best_rank)
                {
                    best_rank = rank;
                    best = cand;
                }
            }
            if (best == m)
            {
                for (size_t cand = 0; cand < m; ++cand)
                {
                    if (!used[cand])
                    {
                        best = cand;
                        break;
                    }
                }
            }
            used[best] = true;
            order.push_back(best);
        }
    }

    std::vector<bool> joined(m, false);
    joined[order[0]] = true;
    PlanNodePtr current = leaves[order[0]];

    for (size_t step = 1; step < m; ++step)
    {
        size_t next = order[step];
        joined[next] = true;
        std::vector<ExprPtr> ready;
        for (auto & jp : join_preds)
        {
            if (jp.placed)
                continue;
            bool all_in = true;
            for (size_t d : jp.deps)
            {
                if (!joined[d])
                {
                    all_in = false;
                    break;
                }
            }
            if (all_in)
            {
                ready.push_back(jp.expr);
                jp.placed = true;
            }
        }
        auto jnode = std::make_shared<JoinNode>();
        jnode->left = current;
        jnode->right = leaves[next];
        jnode->condition = combine_and(ready);
        jnode->join_type = jnode->condition ? popo::parser::JoinType::Inner : popo::parser::JoinType::Cross;
        current = jnode;
    }

    for (const auto & jp : join_preds)
    {
        if (!jp.placed)
            post_filters.push_back(jp.expr);
    }
    if (!post_filters.empty())
    {
        auto f = std::make_shared<FilterNode>();
        f->child = current;
        f->condition = combine_and(post_filters);
        current = f;
    }
    return current;
}

popo::planner::PlanNodePtr
Executor::optimize_plan(const popo::planner::PlanNodePtr & node, bool preserve_star_order) const
{
    if (!node)
        return nullptr;
    using popo::planner::PlanNodeType;
    switch (node->type)
    {
        case PlanNodeType::Project: {
            const auto & p = static_cast<const popo::planner::ProjectNode &>(*node);
            bool has_unqualified_star = false;
            for (const auto & e : p.expressions)
            {
                if (e && e->type == ExprType::Star && static_cast<const popo::parser::StarExpr &>(*e).table_name.empty())
                {
                    has_unqualified_star = true;
                    break;
                }
            }
            auto copy = std::make_shared<popo::planner::ProjectNode>(p);
            copy->child = optimize_plan(p.child, has_unqualified_star);
            return copy;
        }
        case PlanNodeType::Agg: {
            const auto & a = static_cast<const popo::planner::AggNode &>(*node);
            auto copy = std::make_shared<popo::planner::AggNode>(a);
            copy->child = optimize_plan(a.child, false);
            return copy;
        }
        case PlanNodeType::Sort: {
            const auto & s = static_cast<const popo::planner::SortNode &>(*node);
            auto copy = std::make_shared<popo::planner::SortNode>(s);
            copy->child = optimize_plan(s.child, preserve_star_order);
            return copy;
        }
        case PlanNodeType::Limit: {
            const auto & l = static_cast<const popo::planner::LimitNode &>(*node);
            auto copy = std::make_shared<popo::planner::LimitNode>(l);
            copy->child = optimize_plan(l.child, preserve_star_order);
            return copy;
        }
        case PlanNodeType::Distinct: {
            const auto & d = static_cast<const popo::planner::DistinctNode &>(*node);
            auto copy = std::make_shared<popo::planner::DistinctNode>(d);
            copy->child = optimize_plan(d.child, preserve_star_order);
            return copy;
        }
        case PlanNodeType::Window: {
            const auto & w = static_cast<const popo::planner::WindowNode &>(*node);
            auto copy = std::make_shared<popo::planner::WindowNode>(w);
            copy->child = optimize_plan(w.child, preserve_star_order);
            return copy;
        }
        case PlanNodeType::SetOp: {
            const auto & op = static_cast<const popo::planner::SetOpNode &>(*node);
            auto copy = std::make_shared<popo::planner::SetOpNode>(op);
            copy->left = optimize_plan(op.left, false);
            copy->right = optimize_plan(op.right, false);
            return copy;
        }
        case PlanNodeType::SubqueryScan: {
            const auto & sub = static_cast<const popo::planner::SubqueryScanNode &>(*node);
            if (sub.recursive || !sub.cte_name.empty())
                return node;
            auto copy = std::make_shared<popo::planner::SubqueryScanNode>(sub);
            copy->child = optimize_plan(sub.child, false);
            return copy;
        }
        case PlanNodeType::Filter: {
            const auto & f = static_cast<const popo::planner::FilterNode &>(*node);
            // Do not push HAVING below AggNode or WindowNode!
            if (f.child && (f.child->type == PlanNodeType::Agg || f.child->type == PlanNodeType::Window))
            {
                auto copy = std::make_shared<popo::planner::FilterNode>(f);
                copy->child = optimize_plan(f.child, preserve_star_order);
                return copy;
            }
            return optimize_join_group(node, preserve_star_order);
        }
        case PlanNodeType::Join: {
            const auto & j = static_cast<const popo::planner::JoinNode &>(*node);
            if (j.join_type == popo::parser::JoinType::Inner || j.join_type == popo::parser::JoinType::Cross)
            {
                return optimize_join_group(node, preserve_star_order);
            }
            auto copy = std::make_shared<popo::planner::JoinNode>(j);
            copy->left = optimize_plan(j.left, preserve_star_order);
            copy->right = optimize_plan(j.right, preserve_star_order);
            return copy;
        }
        default:
            return node;
    }
}

popo::base::Result<popo::bytecode::ExecuteSuccess>
Executor::execute(const popo::planner::PlanNodePtr & plan, popo::txn::Transaction * outer_txn)
{
    result_ = popo::bytecode::ExecuteSuccess();
    subquery_plans_.clear();
    uncorrelated_subqueries_.clear();
    correlated_subqueries_.clear();
    materialized_.clear();
    stream_materialized_.clear();
    recursive_inputs_.clear();
    spill_stats_ = {};
    memory_ = std::make_unique<MemoryTracker>(std::max<size_t>(options_.memory_limit_bytes, 4096));
    temp_ = std::make_unique<TempFileManager>(options_.temp_directory);
    min_env_depth_seen_ = std::numeric_limits<int>::max();
    own_txn_ = outer_txn == nullptr;
    txn_ = own_txn_ ? engine_.begin_txn() : outer_txn;
    auto fail = [&](popo::base::Error error) -> popo::base::Result<popo::bytecode::ExecuteSuccess>
    {
        if (txn_ && own_txn_)
        {
            engine_.abort_txn(txn_);
        }
        txn_ = nullptr;
        if (memory_)
            spill_stats_.peak_memory_bytes = memory_->peak();
        if (temp_)
        {
            spill_stats_.files_created = temp_->files_created();
            spill_stats_.bytes_written = temp_->bytes_written();
        }
        temp_.reset();
        memory_.reset();
        return std::unexpected(std::move(error));
    };
    if (!plan)
    {
        return fail(popo::base::Error(
            static_cast<int32_t>(popo::base::InvalidArg), "executor received a null plan"));
    }
    engine_.mvcc()->begin_statement(txn_);
    popo::base::Result<void> executed;
    switch (plan->type)
    {
        case popo::planner::PlanNodeType::Insert:
            executed = exec_insert(static_cast<const popo::planner::InsertNode &>(*plan));
            break;
        case popo::planner::PlanNodeType::Update:
            executed = exec_update(static_cast<const popo::planner::UpdateNode &>(*plan));
            break;
        case popo::planner::PlanNodeType::Delete:
            executed = exec_delete(static_cast<const popo::planner::DeleteNode &>(*plan));
            break;
        case popo::planner::PlanNodeType::AlterTable:
            executed = exec_alter_table(static_cast<const popo::planner::AlterTableNode &>(*plan));
            break;
        case popo::planner::PlanNodeType::CreateView:
            executed = exec_create_view(static_cast<const popo::planner::CreateViewNode &>(*plan));
            break;
        case popo::planner::PlanNodeType::DropView:
            executed = exec_drop_view(static_cast<const popo::planner::DropViewNode &>(*plan));
            break;
        default: {
            auto opt_plan = optimize_plan(plan, false);
            auto streamed = stream_rows(
                opt_plan, nullptr,
                [&](StreamItem && item, const Batch &) -> popo::base::Result<bool>
                {
                    result_.rows.push_back(std::make_shared<Row>(std::move(item.row)));
                    return true;
                });
            if (!streamed)
                return fail(streamed.error());
            result_.columns = streamed->flat_names();
            break;
        }
    }
    if (!executed)
        return fail(executed.error());
    if (own_txn_ && txn_)
    {
        auto * open = txn_;
        txn_ = nullptr;
        auto committed = engine_.commit_txn(open);
        if (!committed)
            return std::unexpected(committed.error());
    }
    txn_ = nullptr;

    if (!result_.rows.empty())
    {
        result_.message = std::to_string(result_.rows.size()) + " rows in set";
    }
    else if (result_.rows_affected > 0)
    {
        result_.message = std::to_string(result_.rows_affected) + " rows affected";
    }
    else if (result_.message.empty())
    {
        result_.message = "OK";
    }
    spill_stats_.peak_memory_bytes = memory_ ? memory_->peak() : 0;
    spill_stats_.files_created = temp_ ? temp_->files_created() : 0;
    spill_stats_.bytes_written = temp_ ? temp_->bytes_written() : 0;
    temp_.reset();
    memory_.reset();
    return result_;
}

popo::base::Result<Executor::Batch>
Executor::exec_batch(const popo::planner::PlanNodePtr & node, const EvalEnv * outer)
{
    if (!node)
        return {};
    switch (node->type)
    {
        case popo::planner::PlanNodeType::Scan:
            return exec_scan(static_cast<const popo::planner::ScanNode &>(*node));
        case popo::planner::PlanNodeType::SubqueryScan:
            return exec_subquery_scan(static_cast<const popo::planner::SubqueryScanNode &>(*node), outer);
        case popo::planner::PlanNodeType::Dual: {
            Batch batch;
            batch.rows.emplace_back();
            return batch;
        }
        case popo::planner::PlanNodeType::Filter:
            return exec_filter(static_cast<const popo::planner::FilterNode &>(*node), outer);
        case popo::planner::PlanNodeType::Project:
            return exec_project(static_cast<const popo::planner::ProjectNode &>(*node), outer);
        case popo::planner::PlanNodeType::Join: {
            auto joined = exec_join(
                static_cast<const popo::planner::JoinNode &>(*node), outer);
            if (!joined || !joined->spilled_rows)
                return joined;
            SpillReader reader(*joined->spilled_rows);
            for (;;)
            {
                auto row = reader.next();
                if (!row)
                    return std::unexpected(row.error());
                if (!*row)
                    break;
                joined->rows.push_back(std::move(**row));
            }
            joined->spilled_rows.reset();
            return joined;
        }
        case popo::planner::PlanNodeType::Agg:
            return exec_agg(static_cast<const popo::planner::AggNode &>(*node), outer);
        case popo::planner::PlanNodeType::Sort:
            return exec_sort(static_cast<const popo::planner::SortNode &>(*node), outer);
        case popo::planner::PlanNodeType::Limit:
            return exec_limit(static_cast<const popo::planner::LimitNode &>(*node), outer);
        case popo::planner::PlanNodeType::Distinct:
            return exec_distinct(static_cast<const popo::planner::DistinctNode &>(*node), outer);
        case popo::planner::PlanNodeType::SetOp:
            return exec_set_op(static_cast<const popo::planner::SetOpNode &>(*node), outer);
        case popo::planner::PlanNodeType::Window:
            return exec_window(static_cast<const popo::planner::WindowNode &>(*node), outer);
        case popo::planner::PlanNodeType::RecursiveScan:
            return exec_recursive_scan(static_cast<const popo::planner::RecursiveScanNode &>(*node));
        default:
            return executor_error("unsupported plan node in executor", popo::base::Errno::InternalErr);
    }
}

popo::base::Result<Executor::Batch> Executor::stream_rows(
    const popo::planner::PlanNodePtr & node, const EvalEnv * outer,
    const StreamConsumer & consumer)
{
    if (!node)
        return Batch{};
    using popo::planner::PlanNodeType;
    if (node->type == PlanNodeType::Scan)
    {
        const auto & scan = static_cast<const popo::planner::ScanNode &>(*node);
        auto qr = schema_mgr_.query_table(scan.table_name);
        if (!qr || !*qr)
            return executor_error(
                std::format("Table '{}' not found", scan.table_name),
                popo::base::Errno::TableNotFound);
        Batch layout;
        ColumnSchema cs;
        cs.table = scan.alias.empty() ? scan.table_name : scan.alias;
        cs.alias = scan.alias;
        if (!scan.column_names.empty())
            cs.columns = scan.column_names;
        else
            for (const auto & column : (*qr)->columns_)
                cs.columns.push_back(column.column_name_);
        layout.schemas.push_back(std::move(cs));
        auto cursor = engine_.mvcc()->scan_cursor(txn_, scan.table_name);
        Row row;
        while (cursor.next(row))
        {
            POPO_ASSIGN_OR_RETURN(
                bool keep_going, consumer(StreamItem{std::move(row), {}}, layout));
            if (!keep_going)
                break;
            row = Row{};
        }
        return layout;
    }
    if (node->type == PlanNodeType::Dual)
    {
        Batch layout;
        POPO_ASSIGN_OR_RETURN(bool ignored, consumer(StreamItem{Row{}, {}}, layout));
        (void) ignored;
        return layout;
    }
    if (node->type == PlanNodeType::SubqueryScan)
    {
        const auto & subquery = static_cast<const popo::planner::SubqueryScanNode &>(*node);
        if (subquery.recursive)
        {
            POPO_ASSIGN_OR_RETURN(Batch batch, exec_subquery_scan(subquery, outer));
            Batch layout;
            layout.schemas = batch.schemas;
            for (auto & row : batch.rows)
            {
                POPO_ASSIGN_OR_RETURN(
                    bool keep_going, consumer(StreamItem{std::move(row), {}}, layout));
                if (!keep_going)
                    break;
            }
            return layout;
        }
        auto alias_layout = [&](const Batch & child)
        {
            Batch layout;
            ColumnSchema schema;
            schema.table = subquery.alias;
            schema.alias = subquery.alias;
            schema.columns = subquery.column_names.empty()
                ? child.flat_names() : subquery.column_names;
            layout.schemas.push_back(std::move(schema));
            return layout;
        };
        if (!subquery.cte_name.empty())
        {
            auto cached = stream_materialized_.find(subquery.child.get());
            if (cached != stream_materialized_.end())
            {
                Batch output_layout = alias_layout(cached->second.layout);
                SpillReader reader(cached->second.path);
                for (;;)
                {
                    auto row = reader.next();
                    if (!row)
                        return std::unexpected(row.error());
                    if (!*row)
                        break;
                    POPO_ASSIGN_OR_RETURN(
                        bool keep_going,
                        consumer(StreamItem{std::move(**row), {}}, output_layout));
                    if (!keep_going)
                        break;
                }
                return output_layout;
            }
            if (!temp_)
                return executor_error(
                    "CTE spill context is not initialized",
                    popo::base::Errno::InternalErr);
            auto path = temp_->create("materialized-cte");
            if (!path)
                return std::unexpected(path.error());
            SpillWriter writer(*path, temp_.get());
            Batch output_layout;
            bool downstream_open = true;
            auto child = stream_rows(
                optimize_plan(subquery.child, false), nullptr,
                [&](StreamItem && item, const Batch & child_layout)
                    -> popo::base::Result<bool>
                {
                    if (output_layout.schemas.empty())
                        output_layout = alias_layout(child_layout);
                    POPO_TRY(writer.write(item.row));
                    if (downstream_open)
                    {
                        POPO_ASSIGN_OR_RETURN(
                            downstream_open,
                            consumer(
                                StreamItem{
                                    std::move(item.row), std::move(item.aggs)},
                                output_layout));
                    }
                    // Finish the shared materialization even if this consumer
                    // stopped early, so later references see the complete CTE.
                    return true;
                });
            if (!child)
                return std::unexpected(child.error());
            if (output_layout.schemas.empty())
                output_layout = alias_layout(*child);
            POPO_TRY(writer.close());
            stream_materialized_.emplace(
                subquery.child.get(), StreamMaterialized{*child, *path});
            return output_layout;
        }

        Batch output_layout;
        auto child = stream_rows(
            subquery.child, outer,
            [&](StreamItem && item, const Batch & child_layout)
                -> popo::base::Result<bool>
            {
                if (output_layout.schemas.empty())
                    output_layout = alias_layout(child_layout);
                return consumer(std::move(item), output_layout);
            });
        if (!child)
            return std::unexpected(child.error());
        if (output_layout.schemas.empty())
            output_layout = alias_layout(*child);
        return output_layout;
    }
    if (node->type == PlanNodeType::Filter)
    {
        const auto & filter = static_cast<const popo::planner::FilterNode &>(*node);
        Batch output_layout;
        auto child = stream_rows(
            filter.child, outer,
            [&](StreamItem && item, const Batch & input_layout) -> popo::base::Result<bool>
            {
                output_layout.schemas = input_layout.schemas;
                EvalEnv env{&input_layout, &item.row, outer,
                            item.aggs.empty() ? nullptr : &item.aggs,
                            outer ? outer->depth + 1 : 0};
                POPO_ASSIGN_OR_RETURN(auto condition, eval(filter.condition, env));
                if (!as_bool(condition))
                    return true;
                return consumer(std::move(item), output_layout);
            });
        if (!child)
            return std::unexpected(child.error());
        if (output_layout.schemas.empty())
            output_layout.schemas = child->schemas;
        return output_layout;
    }
    if (node->type == PlanNodeType::Limit)
    {
        const auto & limit = static_cast<const popo::planner::LimitNode &>(*node);
        Batch output_layout;
        int64_t skip = std::max<int64_t>(limit.offset_count, 0);
        int64_t remaining = limit.limit_count;
        auto child = stream_rows(
            limit.child, outer,
            [&](StreamItem && item, const Batch & input_layout) -> popo::base::Result<bool>
            {
                output_layout.schemas = input_layout.schemas;
                if (skip > 0)
                {
                    --skip;
                    return true;
                }
                if (remaining == 0)
                    return false;
                POPO_ASSIGN_OR_RETURN(bool keep_going, consumer(std::move(item), output_layout));
                if (remaining > 0)
                    --remaining;
                return keep_going && remaining != 0;
            });
        if (!child)
            return std::unexpected(child.error());
        if (output_layout.schemas.empty())
            output_layout.schemas = child->schemas;
        return output_layout;
    }
    if (node->type == PlanNodeType::Project)
    {
        const auto & project = static_cast<const popo::planner::ProjectNode &>(*node);
        Batch output_layout;
        std::vector<ExprPtr> outputs;
        std::vector<size_t> star_columns;
        bool prepared = false;
        auto prepare = [&](const Batch & input_layout)
        {
            if (prepared)
                return;
            ColumnSchema cs;
            for (size_t i = 0; i < project.expressions.size(); ++i)
            {
                const auto & expr = project.expressions[i];
                std::string name = i < project.output_names.size() ? project.output_names[i] : "";
                if (expr->type == ExprType::Star)
                {
                    const auto & star = static_cast<const popo::parser::StarExpr &>(*expr);
                    size_t offset = 0;
                    for (const auto & schema : input_layout.schemas)
                    {
                        const bool include = star.table_name.empty()
                            || eq_ci(star.table_name, schema.table)
                            || (!schema.alias.empty() && eq_ci(star.table_name, schema.alias));
                        for (const auto & column : schema.columns)
                        {
                            if (include)
                            {
                                cs.columns.push_back(column);
                                outputs.push_back(nullptr);
                                star_columns.push_back(offset);
                            }
                            ++offset;
                        }
                    }
                    continue;
                }
                if (name.empty())
                    name = expr->type == ExprType::FunctionCall
                        ? agg_display_name(static_cast<const popo::parser::FunctionCallExpr &>(*expr))
                        : std::format("column{}", cs.columns.size());
                cs.columns.push_back(std::move(name));
                outputs.push_back(expr);
            }
            output_layout.schemas.push_back(std::move(cs));
            prepared = true;
        };
        auto child = stream_rows(
            project.child, outer,
            [&](StreamItem && item, const Batch & input_layout) -> popo::base::Result<bool>
            {
                prepare(input_layout);
                EvalEnv env{&input_layout, &item.row, outer,
                            item.aggs.empty() ? nullptr : &item.aggs,
                            outer ? outer->depth + 1 : 0};
                Row projected;
                projected.reserve_values(outputs.size());
                size_t star_index = 0;
                for (const auto & expr : outputs)
                {
                    if (!expr)
                        projected.append_value(item.row.values_[star_columns[star_index++]]);
                    else
                    {
                        POPO_ASSIGN_OR_RETURN(Value value, eval(expr, env));
                        projected.append_value(std::move(value));
                    }
                }
                return consumer(StreamItem{std::move(projected), {}}, output_layout);
            });
        if (!child)
            return std::unexpected(child.error());
        prepare(*child);
        return output_layout;
    }

    if (node->type == PlanNodeType::Join)
    {
        auto joined = exec_join(
            static_cast<const popo::planner::JoinNode &>(*node), outer);
        if (!joined)
            return std::unexpected(joined.error());
        Batch layout;
        layout.schemas = joined->schemas;
        if (joined->spilled_rows)
        {
            SpillReader reader(*joined->spilled_rows);
            for (;;)
            {
                auto row = reader.next();
                if (!row)
                    return std::unexpected(row.error());
                if (!*row)
                    break;
                POPO_ASSIGN_OR_RETURN(
                    bool keep_going,
                    consumer(StreamItem{std::move(**row), {}}, layout));
                if (!keep_going)
                    break;
            }
        }
        else
        {
            for (auto & row : joined->rows)
            {
                POPO_ASSIGN_OR_RETURN(
                    bool keep_going,
                    consumer(StreamItem{std::move(row), {}}, layout));
                if (!keep_going)
                    break;
            }
        }
        return layout;
    }

    // Blocking and uncommon operators retain their implementation but expose a
    // pull-compatible boundary to their parent.
    POPO_ASSIGN_OR_RETURN(Batch batch, exec_batch(node, outer));
    Batch layout;
    layout.schemas = batch.schemas;
    const bool has_aggs = !batch.row_aggs.empty();
    if (batch.spilled_rows)
    {
        SpillReader reader(*batch.spilled_rows);
        for (;;)
        {
            auto row = reader.next();
            if (!row)
                return std::unexpected(row.error());
            if (!*row)
                break;
            POPO_ASSIGN_OR_RETURN(
                bool keep_going,
                consumer(StreamItem{std::move(**row), {}}, layout));
            if (!keep_going)
                break;
        }
        return layout;
    }
    for (size_t i = 0; i < batch.rows.size(); ++i)
    {
        StreamItem item;
        item.row = std::move(batch.rows[i]);
        if (has_aggs)
            item.aggs = std::move(batch.row_aggs[i]);
        POPO_ASSIGN_OR_RETURN(bool keep_going, consumer(std::move(item), layout));
        if (!keep_going)
            break;
    }
    return layout;
}

popo::base::Result<Executor::Batch> Executor::exec_scan(const popo::planner::ScanNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
    {
        return executor_error(std::format("Table '{}' not found", node.table_name), popo::base::Errno::TableNotFound);
    }
    const auto & schema = *qr;
    Batch batch;
    ColumnSchema cs;
    // An alias hides the base name, so emp.dept inside FROM emp e2 resolves outward.
    cs.table = node.alias.empty() ? node.table_name : node.alias;
    cs.alias = node.alias;
    if (!node.column_names.empty())
    {
        cs.columns = node.column_names;
    }
    else
    {
        for (const auto & col : schema->columns_)
        {
            cs.columns.push_back(col.column_name_);
        }
    }
    batch.schemas.push_back(std::move(cs));

    for (auto & row : engine_.mvcc()->scan(txn_, node.table_name))
    {
        batch.rows.push_back(std::move(row));
    }
    return batch;
}

popo::base::Result<Executor::Batch>
Executor::exec_subquery_scan(const popo::planner::SubqueryScanNode & node, const EvalEnv * outer)
{
    auto it = materialized_.find(node.child.get());
    if (it != materialized_.end())
    {
        Batch batch;
        ColumnSchema cs;
        cs.table = node.alias;
        cs.alias = node.alias;
        cs.columns = node.column_names.empty() ? it->second.flat_names() : node.column_names;
        batch.schemas.push_back(std::move(cs));
        batch.rows = it->second.rows;
        return batch;
    }

    if (node.recursive || !node.cte_name.empty() || outer == nullptr)
    {
        auto child_plan = (!node.recursive && !node.cte_name.empty()) ? optimize_plan(node.child, false) : node.child;
        popo::base::Result<Batch> built_result =
            node.recursive ? exec_recursive(node) : exec_batch(child_plan, nullptr);
        if (!built_result)
            return std::unexpected(built_result.error());
        Batch built = std::move(*built_result);
        it = materialized_.emplace(node.child.get(), std::move(built)).first;
        Batch batch;
        ColumnSchema cs;
        cs.table = node.alias;
        cs.alias = node.alias;
        cs.columns = node.column_names.empty() ? it->second.flat_names() : node.column_names;
        batch.schemas.push_back(std::move(cs));
        batch.rows = it->second.rows;
        return batch;
    }

    int saved = min_env_depth_seen_;
    min_env_depth_seen_ = std::numeric_limits<int>::max();
    POPO_ASSIGN_OR_RETURN(Batch built, exec_batch(node.child, outer));
    bool correlated = min_env_depth_seen_ <= outer->depth;
    min_env_depth_seen_ = std::min(saved, min_env_depth_seen_);

    Batch batch;
    ColumnSchema cs;
    cs.table = node.alias;
    cs.alias = node.alias;
    cs.columns = node.column_names.empty() ? built.flat_names() : node.column_names;
    batch.schemas.push_back(std::move(cs));
    if (!correlated)
    {
        it = materialized_.emplace(node.child.get(), std::move(built)).first;
        batch.rows = it->second.rows;
    }
    else
    {
        batch.rows = std::move(built.rows);
    }
    return batch;
}

popo::base::Result<Executor::Batch>
Executor::exec_filter(const popo::planner::FilterNode & node, const EvalEnv * outer)
{
    POPO_ASSIGN_OR_RETURN(auto batch, exec_batch(node.child, outer));
    Batch out;
    out.schemas = batch.schemas;
    bool has_aggs = !batch.row_aggs.empty();
    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;
    env.depth = outer ? outer->depth + 1 : 0;
    for (size_t i = 0; i < batch.rows.size(); ++i)
    {
        env.row = &batch.rows[i];
        env.aggs = has_aggs ? &batch.row_aggs[i] : nullptr;
        POPO_ASSIGN_OR_RETURN(auto condition, eval(node.condition, env));
        if (as_bool(condition))
        {
            out.rows.push_back(std::move(batch.rows[i]));
            if (has_aggs)
            {
                out.row_aggs.push_back(std::move(batch.row_aggs[i]));
            }
        }
    }
    return out;
}

popo::base::Result<Executor::Batch>
Executor::exec_project(const popo::planner::ProjectNode & node, const EvalEnv * outer)
{
    POPO_ASSIGN_OR_RETURN(auto batch, exec_batch(node.child, outer));
    Batch out;
    ColumnSchema cs;
    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;
    env.depth = outer ? outer->depth + 1 : 0;

    // Expand stars once to build the output schema. A null expr marks a passthrough
    // column whose input index is stored in star_columns.
    std::vector<ExprPtr> outputs;
    std::vector<size_t> star_columns;
    for (size_t i = 0; i < node.expressions.size(); ++i)
    {
        const auto & expr = node.expressions[i];
        std::string name = i < node.output_names.size() ? node.output_names[i] : "";
        if (expr->type == ExprType::Star)
        {
            const auto & star = static_cast<const popo::parser::StarExpr &>(*expr);
            size_t offset = 0;
            for (const auto & s : batch.schemas)
            {
                bool include = star.table_name.empty() || eq_ci(star.table_name, s.table)
                    || (!s.alias.empty() && eq_ci(star.table_name, s.alias));
                for (const auto & col : s.columns)
                {
                    if (include)
                    {
                        cs.columns.push_back(col);
                        outputs.push_back(nullptr);
                        star_columns.push_back(offset);
                    }
                    ++offset;
                }
            }
            continue;
        }
        if (name.empty())
        {
            if (expr->type == ExprType::FunctionCall)
            {
                name = agg_display_name(static_cast<const popo::parser::FunctionCallExpr &>(*expr));
            }
            else
            {
                name = std::format("column{}", cs.columns.size());
            }
        }
        cs.columns.push_back(name);
        outputs.push_back(expr);
    }
    out.schemas.push_back(std::move(cs));

    bool has_aggs = !batch.row_aggs.empty();
    size_t star_idx = 0;
    for (size_t r = 0; r < batch.rows.size(); ++r)
    {
        const auto & row = batch.rows[r];
        env.row = &row;
        env.aggs = has_aggs ? &batch.row_aggs[r] : nullptr;
        Row projected;
        projected.reserve_values(outputs.size());
        star_idx = 0;
        for (const auto & expr : outputs)
        {
            if (expr == nullptr)
            {
                projected.append_value(row.values_[star_columns[star_idx++]]);
            }
            else
            {
                POPO_ASSIGN_OR_RETURN(auto value, eval(expr, env));
                projected.append_value(std::move(value));
            }
        }
        out.rows.push_back(std::move(projected));
    }
    return out;
}

popo::base::Result<Executor::Batch>
Executor::exec_join(const popo::planner::JoinNode & node, const EvalEnv * outer)
{
    struct JoinInput
    {
        Batch batch;
        std::optional<std::filesystem::path> spill_path;
        size_t charged = 0;
    };
    const bool spillable_join = node.join_type == popo::parser::JoinType::Inner
        || node.join_type == popo::parser::JoinType::Left;
    auto collect = [&](const popo::planner::PlanNodePtr & child) -> popo::base::Result<JoinInput>
    {
        JoinInput input;
        std::unique_ptr<SpillWriter> writer;
        auto streamed = stream_rows(
            child, outer,
            [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
            {
                input.batch.schemas = layout.schemas;
                const size_t bytes = estimate_row_bytes(item.row) + 32;
                if (!spillable_join)
                {
                    input.batch.rows.push_back(std::move(item.row));
                    return true;
                }
                if (!writer && memory_ && memory_->try_reserve(bytes))
                {
                    input.charged += bytes;
                    input.batch.rows.push_back(std::move(item.row));
                    return true;
                }
                if (!writer)
                {
                    if (!temp_)
                        return executor_error(
                            "join spill context is not initialized",
                            popo::base::Errno::InternalErr);
                    auto path = temp_->create("join-input");
                    if (!path)
                        return std::unexpected(path.error());
                    input.spill_path = *path;
                    writer = std::make_unique<SpillWriter>(*path, temp_.get());
                    for (const auto & row : input.batch.rows)
                        POPO_TRY(writer->write(row));
                    input.batch.rows.clear();
                    if (memory_)
                        memory_->release(input.charged);
                    input.charged = 0;
                }
                POPO_TRY(writer->write(item.row));
                return true;
            });
        if (!streamed)
            return std::unexpected(streamed.error());
        input.batch.schemas = streamed->schemas;
        if (writer)
            POPO_TRY(writer->close());
        return input;
    };
    POPO_ASSIGN_OR_RETURN(JoinInput left_input, collect(node.left));
    POPO_ASSIGN_OR_RETURN(JoinInput right_input, collect(node.right));
    struct ChargeGuard
    {
        MemoryTracker * memory;
        size_t left;
        size_t right;
        ~ChargeGuard()
        {
            if (memory)
                memory->release(left + right);
        }
    } charge_guard{memory_.get(), left_input.charged, right_input.charged};
    Batch & left = left_input.batch;
    Batch & right = right_input.batch;
    Batch out;
    out.schemas = left.schemas;
    out.schemas.insert(out.schemas.end(), right.schemas.begin(), right.schemas.end());
    if (!temp_)
        return executor_error(
            "join output spill context is not initialized",
            popo::base::Errno::InternalErr);
    auto join_output_path = temp_->create("join-output");
    if (!join_output_path)
        return std::unexpected(join_output_path.error());
    out.spilled_rows = *join_output_path;
    SpillWriter join_output(*join_output_path, temp_.get());
    auto emit_join = [&](Row row) { return join_output.write(row); };

    size_t right_width = right.width();
    Row null_right;
    null_right.reserve_values(right_width);
    for (size_t i = 0; i < right_width; ++i)
    {
        null_right.append_value(Value());
    }
    Row null_left;
    null_left.reserve_values(left.width());
    for (size_t i = 0; i < left.width(); ++i)
    {
        null_left.append_value(Value());
    }

    EvalEnv env;
    env.layout = &out;
    env.outer = outer;
    env.depth = outer ? outer->depth + 1 : 0;
    auto concat = [](const Row & l, const Row & r) {
        Row row;
        row.reserve_values(l.values_.size() + r.values_.size());
        row.values_ = l.values_;
        row.values_.insert(row.values_.end(), r.values_.begin(), r.values_.end());
        return row;
    };
    auto matches = [&](const Row & l, const Row & r) -> popo::base::Result<bool> {
        if (!node.condition)
            return true;
        Row joined = concat(l, r);
        env.row = &joined;
        POPO_ASSIGN_OR_RETURN(auto condition, eval(node.condition, env));
        return as_bool(condition);
    };

    bool emit_unmatched_left = node.join_type == popo::parser::JoinType::Left || node.join_type == popo::parser::JoinType::Full;
    bool emit_unmatched_right = node.join_type == popo::parser::JoinType::Right || node.join_type == popo::parser::JoinType::Full;
    std::vector<bool> right_matched(right.rows.size(), false);

    // Extract equijoin key pairs (left_expr = right_expr) for hash join.
    std::vector<std::pair<ExprPtr, ExprPtr>> equi_keys;
    if (node.condition)
    {
        auto in_schemas = [](const std::vector<ColumnSchema> & schemas, const std::string & tbl, const std::string & col)
        {
            for (const auto & s : schemas)
            {
                bool tm = tbl.empty() || eq_ci(tbl, s.table) || (!s.alias.empty() && eq_ci(tbl, s.alias));
                if (!tm)
                    continue;
                for (const auto & c : s.columns)
                {
                    if (eq_ci(c, col))
                        return true;
                }
            }
            return false;
        };
        auto side_of = [&](const ExprPtr & side) -> int
        {
            // Returns 1 if references left only, 2 if right only, 0 otherwise.
            if (!side || expr_has_subquery_or_agg(side))
                return 0;
            std::vector<const popo::parser::ColumnRefExpr *> refs;
            collect_col_refs(side, refs);
            bool has_l = false;
            bool has_r = false;
            for (const auto * ref : refs)
            {
                bool in_l = in_schemas(left.schemas, ref->table_name, ref->column_name);
                bool in_r = in_schemas(right.schemas, ref->table_name, ref->column_name);
                if (in_l && in_r)
                    return 0;
                if (in_l)
                    has_l = true;
                if (in_r)
                    has_r = true;
            }
            if (has_l && !has_r)
                return 1;
            if (has_r && !has_l)
                return 2;
            return 0;
        };
        std::vector<ExprPtr> parts;
        split_and_conjuncts(node.condition, parts);
        for (const auto & p : parts)
        {
            if (!p || p->type != ExprType::BinaryOp)
                continue;
            const auto & b = static_cast<const popo::parser::BinaryOpExpr &>(*p);
            if (b.op != popo::parser::BinaryOpType::Eq)
                continue;
            int sl = side_of(b.left);
            int sr = side_of(b.right);
            if (sl == 1 && sr == 2)
                equi_keys.emplace_back(b.left, b.right);
            else if (sl == 2 && sr == 1)
                equi_keys.emplace_back(b.right, b.left);
        }
    }

    if (!equi_keys.empty())
    {
        EvalEnv left_env{&left, nullptr, outer, nullptr, env.depth};
        EvalEnv right_env{&right, nullptr, outer, nullptr, env.depth};
        size_t build_bytes = 0;
        for (const auto & row : right.rows)
            build_bytes += estimate_row_bytes(row) + 48;
        const bool can_grace_spill = memory_ && temp_
            && (node.join_type == popo::parser::JoinType::Inner
                || node.join_type == popo::parser::JoinType::Left)
            && (left_input.spill_path || right_input.spill_path
                || build_bytes > std::max<size_t>(memory_->limit() / 2, 4096));
        if (can_grace_spill)
        {
            auto decorate = [&](Row row, EvalEnv & key_env, bool left_side)
                -> popo::base::Result<Row>
            {
                key_env.row = &row;
                std::string key;
                bool any_null = false;
                for (const auto & [lexpr, rexpr] : equi_keys)
                {
                    POPO_ASSIGN_OR_RETURN(
                        Value value, eval(left_side ? lexpr : rexpr, key_env));
                    if (is_null(value))
                    {
                        any_null = true;
                        break;
                    }
                    key += value_key(value);
                    key.push_back('\x1f');
                }
                row.append_value(Value(ValueType::String, any_null ? std::string() : key));
                return row;
            };
            constexpr size_t partitions = 32;
            size_t left_pos = 0;
            size_t right_pos = 0;
            std::unique_ptr<SpillReader> left_source;
            std::unique_ptr<SpillReader> right_source;
            if (left_input.spill_path)
                left_source = std::make_unique<SpillReader>(*left_input.spill_path);
            if (right_input.spill_path)
                right_source = std::make_unique<SpillReader>(*right_input.spill_path);
            auto next_raw = [](Batch & batch, size_t & position, std::unique_ptr<SpillReader> & reader)
                -> popo::base::Result<std::optional<Row>>
            {
                if (reader)
                    return reader->next();
                if (position == batch.rows.size())
                    return std::optional<Row>{};
                return std::optional<Row>(std::move(batch.rows[position++]));
            };
            auto hash_decorated = [](const Row & row)
            {
                const auto & key = row.values_.back().string_ref();
                return std::hash<std::string>{}(key);
            };
            auto left_paths = partition_rows(
                [&]() -> popo::base::Result<std::optional<Row>>
                {
                    auto row = next_raw(left, left_pos, left_source);
                    if (!row)
                        return std::unexpected(row.error());
                    if (!*row)
                        return std::optional<Row>{};
                    POPO_ASSIGN_OR_RETURN(
                        Row decorated, decorate(std::move(**row), left_env, true));
                    return std::optional<Row>(std::move(decorated));
                },
                hash_decorated, partitions, *temp_, "join-left");
            if (!left_paths)
                return std::unexpected(left_paths.error());
            auto right_paths = partition_rows(
                [&]() -> popo::base::Result<std::optional<Row>>
                {
                    auto row = next_raw(right, right_pos, right_source);
                    if (!row)
                        return std::unexpected(row.error());
                    if (!*row)
                        return std::optional<Row>{};
                    POPO_ASSIGN_OR_RETURN(
                        Row decorated, decorate(std::move(**row), right_env, false));
                    return std::optional<Row>(std::move(decorated));
                },
                hash_decorated, partitions, *temp_, "join-right");
            if (!right_paths)
                return std::unexpected(right_paths.error());
            left.rows.clear();
            right.rows.clear();
            memory_->release(charge_guard.left + charge_guard.right);
            charge_guard.left = 0;
            charge_guard.right = 0;

            for (size_t partition = 0; partition < partitions; ++partition)
            {
                std::vector<Row> build;
                std::unordered_map<std::string, std::vector<size_t>> hash_table;
                size_t charged = 0;
                bool skewed = false;
                SpillReader right_reader((*right_paths)[partition]);
                for (;;)
                {
                    auto item = right_reader.next();
                    if (!item)
                        return std::unexpected(item.error());
                    if (!*item)
                        break;
                    const size_t bytes = estimate_row_bytes(**item) + 48;
                    if (!memory_->try_reserve(bytes))
                    {
                        skewed = true;
                        break;
                    }
                    charged += bytes;
                    build.push_back(std::move(**item));
                }
                if (!skewed)
                {
                    for (size_t i = 0; i < build.size(); ++i)
                    {
                        const std::string & key = build[i].values_.back().string_ref();
                        if (!key.empty())
                            hash_table[key].push_back(i);
                    }
                }
                SpillReader left_reader((*left_paths)[partition]);
                for (;;)
                {
                    auto left_item = left_reader.next();
                    if (!left_item)
                    {
                        memory_->release(charged);
                        return std::unexpected(left_item.error());
                    }
                    if (!*left_item)
                        break;
                    Row left_row = std::move(**left_item);
                    const std::string left_key = left_row.values_.back().string_ref();
                    left_row.values_.pop_back();
                    bool matched = false;
                    if (!left_key.empty() && !skewed)
                    {
                        auto found = hash_table.find(left_key);
                        if (found != hash_table.end())
                        {
                            for (size_t index : found->second)
                            {
                                Row & candidate = build[index];
                                Value saved = std::move(candidate.values_.back());
                                candidate.values_.pop_back();
                                POPO_ASSIGN_OR_RETURN(bool pair, matches(left_row, candidate));
                                if (pair)
                                {
                                    matched = true;
                                    POPO_TRY(emit_join(concat(left_row, candidate)));
                                }
                                candidate.append_value(std::move(saved));
                            }
                        }
                    }
                    else if (!left_key.empty())
                    {
                        // A partition that remains larger than the budget after
                        // hashing is skewed. Block nested-loop is slow but has a
                        // strict one-row memory footprint and guarantees progress.
                        SpillReader fallback((*right_paths)[partition]);
                        for (;;)
                        {
                            auto right_item = fallback.next();
                            if (!right_item)
                            {
                                memory_->release(charged);
                                return std::unexpected(right_item.error());
                            }
                            if (!*right_item)
                                break;
                            Row right_row = std::move(**right_item);
                            const std::string right_key = right_row.values_.back().string_ref();
                            right_row.values_.pop_back();
                            if (right_key != left_key)
                                continue;
                            POPO_ASSIGN_OR_RETURN(bool pair, matches(left_row, right_row));
                            if (pair)
                            {
                                matched = true;
                                POPO_TRY(emit_join(concat(left_row, right_row)));
                            }
                        }
                    }
                    if (!matched && emit_unmatched_left)
                        POPO_TRY(emit_join(concat(left_row, null_right)));
                }
                memory_->release(charged);
            }
            POPO_TRY(join_output.close());
            return out;
        }
        std::unordered_map<std::string, std::vector<size_t>> hash_table;
        hash_table.reserve(right.rows.size());
        for (size_t j = 0; j < right.rows.size(); ++j)
        {
            right_env.row = &right.rows[j];
            std::string key;
            bool any_null = false;
            for (const auto & [_, rexpr] : equi_keys)
            {
                POPO_ASSIGN_OR_RETURN(Value rv, eval(rexpr, right_env));
                if (is_null(rv))
                {
                    any_null = true;
                    break;
                }
                key += value_key(rv);
                key += '\x1f';
            }
            if (!any_null)
            {
                hash_table[key].push_back(j);
            }
        }
        for (const auto & lrow : left.rows)
        {
            left_env.row = &lrow;
            std::string key;
            bool any_null = false;
            for (const auto & [lexpr, _] : equi_keys)
            {
                POPO_ASSIGN_OR_RETURN(Value lv, eval(lexpr, left_env));
                if (is_null(lv))
                {
                    any_null = true;
                    break;
                }
                key += value_key(lv);
                key += '\x1f';
            }
            bool matched = false;
            if (!any_null)
            {
                auto it = hash_table.find(key);
                if (it != hash_table.end())
                {
                    for (size_t j : it->second)
                    {
                        POPO_ASSIGN_OR_RETURN(bool matched_pair, matches(lrow, right.rows[j]));
                        if (matched_pair)
                        {
                            matched = true;
                            right_matched[j] = true;
                            POPO_TRY(emit_join(concat(lrow, right.rows[j])));
                        }
                    }
                }
            }
            if (!matched && emit_unmatched_left)
            {
                POPO_TRY(emit_join(concat(lrow, null_right)));
            }
        }
        if (emit_unmatched_right)
        {
            for (size_t j = 0; j < right.rows.size(); ++j)
            {
                if (!right_matched[j])
                {
                    POPO_TRY(emit_join(concat(null_left, right.rows[j])));
                }
            }
        }
        POPO_TRY(join_output.close());
        return out;
    }

    if (left_input.spill_path || right_input.spill_path)
    {
        size_t left_position = 0;
        std::unique_ptr<SpillReader> left_reader;
        if (left_input.spill_path)
            left_reader = std::make_unique<SpillReader>(*left_input.spill_path);
        for (;;)
        {
            std::optional<Row> left_item;
            if (left_reader)
            {
                auto item = left_reader->next();
                if (!item)
                    return std::unexpected(item.error());
                left_item = std::move(*item);
            }
            else if (left_position < left.rows.size())
                left_item = left.rows[left_position++];
            if (!left_item)
                break;
            bool matched = false;
            if (right_input.spill_path)
            {
                SpillReader right_reader(*right_input.spill_path);
                for (;;)
                {
                    auto right_item = right_reader.next();
                    if (!right_item)
                        return std::unexpected(right_item.error());
                    if (!*right_item)
                        break;
                    POPO_ASSIGN_OR_RETURN(bool pair, matches(*left_item, **right_item));
                    if (pair)
                    {
                        matched = true;
                        POPO_TRY(emit_join(concat(*left_item, **right_item)));
                    }
                }
            }
            else
            {
                for (const auto & right_row : right.rows)
                {
                    POPO_ASSIGN_OR_RETURN(bool pair, matches(*left_item, right_row));
                    if (pair)
                    {
                        matched = true;
                        POPO_TRY(emit_join(concat(*left_item, right_row)));
                    }
                }
            }
            if (!matched && emit_unmatched_left)
                POPO_TRY(emit_join(concat(*left_item, null_right)));
        }
        POPO_TRY(join_output.close());
        return out;
    }

    for (const auto & lrow : left.rows)
    {
        bool matched = false;
        for (size_t j = 0; j < right.rows.size(); ++j)
        {
            POPO_ASSIGN_OR_RETURN(bool matched_pair, matches(lrow, right.rows[j]));
            if (matched_pair)
            {
                matched = true;
                right_matched[j] = true;
                POPO_TRY(emit_join(concat(lrow, right.rows[j])));
            }
        }
        if (!matched && emit_unmatched_left)
        {
            POPO_TRY(emit_join(concat(lrow, null_right)));
        }
    }
    if (emit_unmatched_right)
    {
        for (size_t j = 0; j < right.rows.size(); ++j)
        {
            if (!right_matched[j])
            {
                POPO_TRY(emit_join(concat(null_left, right.rows[j])));
            }
        }
    }
    POPO_TRY(join_output.close());
    return out;
}

popo::base::Result<Executor::Batch>
Executor::exec_agg(const popo::planner::AggNode & node, const EvalEnv * outer)
{
    if (!memory_ || !temp_)
        return executor_error("aggregation spill context is not initialized", popo::base::Errno::InternalErr);
    Batch out;
    struct AggState
    {
        int64_t count = 0;
        Value result;
        bool initialized = false;
        std::set<std::string> distinct_seen;
        std::string last_distinct;
        bool has_last_distinct = false;
    };
    const size_t group_count = node.group_by.size();
    const size_t agg_count = node.agg_exprs.size();
    size_t row_width = 0;
    size_t primary_distinct = agg_count;
    for (size_t i = 0; i < agg_count; ++i)
    {
        const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
        if (func.is_distinct)
        {
            primary_distinct = i;
            break;
        }
    }
    auto path = temp_->create("agg-input");
    if (!path)
        return std::unexpected(path.error());
    SpillWriter spool(*path, temp_.get());
    auto streamed = stream_rows(
        node.child, outer,
        [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
        {
            out.schemas = layout.schemas;
            row_width = item.row.values_.size();
            EvalEnv env{&layout, &item.row, outer,
                        item.aggs.empty() ? nullptr : &item.aggs,
                        outer ? outer->depth + 1 : 0};
            Row decorated;
            decorated.reserve_values(group_count + agg_count + row_width);
            for (const auto & group : node.group_by)
            {
                POPO_ASSIGN_OR_RETURN(Value value, eval(group, env));
                decorated.append_value(std::move(value));
            }
            for (const auto & expression : node.agg_exprs)
            {
                const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*expression);
                const bool star = func.args.empty() || func.args[0]->type == ExprType::Star;
                if (star)
                    decorated.append_value(Value(ValueType::Int32, 1));
                else
                {
                    POPO_ASSIGN_OR_RETURN(Value value, eval(func.args[0], env));
                    decorated.append_value(std::move(value));
                }
            }
            for (auto & value : item.row.values_)
                decorated.append_value(std::move(value));
            POPO_TRY(spool.write(decorated));
            return true;
        });
    if (!streamed)
        return std::unexpected(streamed.error());
    out.schemas = streamed->schemas;
    row_width = streamed->width();
    POPO_TRY(spool.close());
    SpillReader input(*path);
    auto less = [&](const Row & a, const Row & b)
    {
        for (size_t i = 0; i < group_count; ++i)
        {
            if (sql_eq(a.values_[i], b.values_[i]) || (is_null(a.values_[i]) && is_null(b.values_[i])))
                continue;
            if (is_null(a.values_[i]))
                return true;
            if (is_null(b.values_[i]))
                return false;
            return sql_lt(a.values_[i], b.values_[i]);
        }
        // Sorting one DISTINCT argument inside each group makes the common
        // COUNT(DISTINCT x) case constant-memory (TPC-H Q16).
        if (primary_distinct < agg_count)
        {
            const auto & av = a.values_[group_count + primary_distinct];
            const auto & bv = b.values_[group_count + primary_distinct];
            if (!sql_eq(av, bv) && !(is_null(av) && is_null(bv)))
            {
                if (is_null(av))
                    return true;
                if (is_null(bv))
                    return false;
                return sql_lt(av, bv);
            }
        }
        return false;
    };
    auto sorted = external_sort([&]() { return input.next(); }, less, *memory_, *temp_);
    if (!sorted)
        return std::unexpected(sorted.error());

    std::vector<AggState> states(agg_count);
    Row representative;
    std::string current_group;
    bool have_group = false;
    auto emit_group = [&]() -> popo::base::Result<void>
    {
        if (!have_group)
            return {};
        std::unordered_map<const Expr *, Value> agg_values;
        for (size_t i = 0; i < agg_count; ++i)
        {
            const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
            const auto & state = states[i];
            Value value;
            switch (func.func)
            {
                case popo::parser::AggFuncType::Count:
                    value = Value(ValueType::Int64, state.count);
                    break;
                case popo::parser::AggFuncType::Avg:
                    if (state.count > 0)
                        value = Value(ValueType::Float64, state.result.to_f64() / static_cast<double>(state.count));
                    break;
                default:
                    value = state.result;
                    break;
            }
            agg_values.emplace(node.agg_exprs[i].get(), value);
        }
        out.rows.push_back(representative);
        out.row_aggs.push_back(std::move(agg_values));
        return {};
    };
    for (auto & decorated : *sorted)
    {
        std::string group_key;
        for (size_t i = 0; i < group_count; ++i)
        {
            group_key += value_key(decorated.values_[i]);
            group_key.push_back('\x1f');
        }
        if (!have_group || group_key != current_group)
        {
            POPO_TRY(emit_group());
            current_group = std::move(group_key);
            have_group = true;
            states.assign(agg_count, AggState{});
            representative = Row{};
            representative.reserve_values(row_width);
            for (size_t i = 0; i < row_width; ++i)
                representative.append_value(decorated.values_[group_count + agg_count + i]);
        }
        for (size_t i = 0; i < agg_count; ++i)
        {
            const auto & func = static_cast<const popo::parser::FunctionCallExpr &>(*node.agg_exprs[i]);
            auto & state = states[i];
            const Value & val = decorated.values_[group_count + i];
            const bool star = func.args.empty() || func.args[0]->type == ExprType::Star;
            if (!star && is_null(val))
                continue;
            if (func.is_distinct)
            {
                const std::string key = value_key(val);
                if (i == primary_distinct)
                {
                    if (state.has_last_distinct && state.last_distinct == key)
                        continue;
                    state.last_distinct = key;
                    state.has_last_distinct = true;
                }
                else if (!state.distinct_seen.insert(key).second)
                    continue;
            }
            switch (func.func)
            {
                case popo::parser::AggFuncType::Count: ++state.count; break;
                case popo::parser::AggFuncType::Sum:
                case popo::parser::AggFuncType::Avg:
                    if (state.initialized)
                    {
                        POPO_ASSIGN_OR_RETURN(state.result, do_arithmetic('+', state.result, val));
                    }
                    else
                        state.result = val;
                    state.initialized = true;
                    ++state.count;
                    break;
                case popo::parser::AggFuncType::Min:
                    if (!state.initialized || sql_lt(val, state.result))
                    {
                        state.result = val;
                        state.initialized = true;
                    }
                    break;
                case popo::parser::AggFuncType::Max:
                    if (!state.initialized || sql_lt(state.result, val))
                    {
                        state.result = val;
                        state.initialized = true;
                    }
                    break;
                case popo::parser::AggFuncType::XmlAgg: {
                    const std::string piece = val.datatype() == ValueType::String
                        ? std::string(val.string_ref()) : val.to_val_string();
                    state.result = Value(
                        ValueType::String,
                        state.initialized ? state.result.string_ref() + piece : piece);
                    state.initialized = true;
                    break;
                }
            }
        }
    }
    POPO_TRY(emit_group());
    if (!have_group && group_count == 0)
    {
        have_group = true;
        representative = Row{};
        representative.reserve_values(row_width);
        for (size_t i = 0; i < row_width; ++i)
            representative.append_value(Value());
        POPO_TRY(emit_group());
    }
    return out;
}

popo::base::Result<Executor::Batch>
Executor::exec_sort(const popo::planner::SortNode & node, const EvalEnv * outer)
{
    if (!memory_ || !temp_)
        return executor_error("sort spill context is not initialized", popo::base::Errno::InternalErr);

    auto spool_path = temp_->create("sort-input");
    if (!spool_path)
        return std::unexpected(spool_path.error());
    SpillWriter spool(*spool_path, temp_.get());
    Batch input_layout;
    std::vector<const Expr *> aggregate_exprs;
    const size_t key_count = node.order_by.size();
    auto streamed = stream_rows(
        node.child, outer,
        [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
        {
            input_layout.schemas = layout.schemas;
            if (aggregate_exprs.empty() && !item.aggs.empty())
                for (const auto & [expr, _] : item.aggs)
                    aggregate_exprs.push_back(expr);
            EvalEnv env{&layout, &item.row, outer,
                        item.aggs.empty() ? nullptr : &item.aggs,
                        outer ? outer->depth + 1 : 0};
            Row decorated;
            decorated.reserve_values(key_count + aggregate_exprs.size() + item.row.values_.size());
            for (const auto & order : node.order_by)
            {
                POPO_ASSIGN_OR_RETURN(Value key, eval(order.expr, env));
                decorated.append_value(std::move(key));
            }
            for (const Expr * expr : aggregate_exprs)
            {
                auto it = item.aggs.find(expr);
                decorated.append_value(it == item.aggs.end() ? Value() : it->second);
            }
            for (auto & value : item.row.values_)
                decorated.append_value(std::move(value));
            POPO_TRY(spool.write(decorated));
            return true;
        });
    if (!streamed)
        return std::unexpected(streamed.error());
    input_layout.schemas = streamed->schemas;
    POPO_TRY(spool.close());

    SpillReader input(*spool_path);
    auto less = [&](const Row & a, const Row & b)
    {
        for (size_t k = 0; k < key_count; ++k)
        {
            const auto & av = a.values_[k];
            const auto & bv = b.values_[k];
            const bool asc = node.order_by[k].ascending;
            if (is_null(av) || is_null(bv))
            {
                if (is_null(av) && is_null(bv))
                    continue;
                return asc ? is_null(av) : is_null(bv);
            }
            if (sql_eq(av, bv))
                continue;
            return asc ? sql_lt(av, bv) : sql_lt(bv, av);
        }
        return false;
    };
    auto sorted = external_sort(
        [&]() { return input.next(); }, less, *memory_, *temp_);
    if (!sorted)
        return std::unexpected(sorted.error());

    Batch out;
    out.schemas = input_layout.schemas;
    const size_t prefix = key_count + aggregate_exprs.size();
    for (auto & decorated : *sorted)
    {
        Row row;
        row.reserve_values(decorated.values_.size() - prefix);
        for (size_t i = prefix; i < decorated.values_.size(); ++i)
            row.append_value(std::move(decorated.values_[i]));
        out.rows.push_back(std::move(row));
        if (!aggregate_exprs.empty())
        {
            std::unordered_map<const Expr *, Value> aggs;
            for (size_t i = 0; i < aggregate_exprs.size(); ++i)
                aggs.emplace(aggregate_exprs[i], std::move(decorated.values_[key_count + i]));
            out.row_aggs.push_back(std::move(aggs));
        }
    }
    return out;
}

popo::base::Result<Executor::Batch>
Executor::exec_limit(const popo::planner::LimitNode & node, const EvalEnv * outer)
{
    POPO_ASSIGN_OR_RETURN(auto batch, exec_batch(node.child, outer));
    Batch out;
    out.schemas = batch.schemas;
    bool has_aggs = !batch.row_aggs.empty();
    int64_t offset = std::max<int64_t>(node.offset_count, 0);
    int64_t remaining = node.limit_count < 0 ? static_cast<int64_t>(batch.rows.size()) : node.limit_count;
    for (size_t i = static_cast<size_t>(offset); i < batch.rows.size() && remaining > 0; ++i, --remaining)
    {
        out.rows.push_back(std::move(batch.rows[i]));
        if (has_aggs)
        {
            out.row_aggs.push_back(std::move(batch.row_aggs[i]));
        }
    }
    return out;
}

popo::base::Result<Executor::Batch>
Executor::exec_distinct(const popo::planner::DistinctNode & node, const EvalEnv * outer)
{
    if (!memory_ || !temp_)
        return executor_error("distinct spill context is not initialized", popo::base::Errno::InternalErr);
    Batch out;
    std::vector<const Expr *> aggregate_exprs;
    size_t row_width = 0;
    auto path = temp_->create("distinct-input");
    if (!path)
        return std::unexpected(path.error());
    SpillWriter spool(*path, temp_.get());
    auto streamed = stream_rows(
        node.child, outer,
        [&](StreamItem && item, const Batch & layout) -> popo::base::Result<bool>
        {
            out.schemas = layout.schemas;
            row_width = item.row.values_.size();
            if (aggregate_exprs.empty() && !item.aggs.empty())
                for (const auto & [expr, _] : item.aggs)
                    aggregate_exprs.push_back(expr);
            for (const Expr * expr : aggregate_exprs)
            {
                auto it = item.aggs.find(expr);
                item.row.append_value(it == item.aggs.end() ? Value() : it->second);
            }
            POPO_TRY(spool.write(item.row));
            return true;
        });
    if (!streamed)
        return std::unexpected(streamed.error());
    out.schemas = streamed->schemas;
    POPO_TRY(spool.close());
    SpillReader input(*path);
    auto less = [&](const Row & a, const Row & b)
    {
        for (size_t i = 0; i < row_width; ++i)
        {
            if (sql_eq(a.values_[i], b.values_[i]) || (is_null(a.values_[i]) && is_null(b.values_[i])))
                continue;
            if (is_null(a.values_[i]))
                return true;
            if (is_null(b.values_[i]))
                return false;
            return sql_lt(a.values_[i], b.values_[i]);
        }
        return false;
    };
    auto sorted = external_sort([&]() { return input.next(); }, less, *memory_, *temp_);
    if (!sorted)
        return std::unexpected(sorted.error());
    std::string previous;
    bool have_previous = false;
    for (auto & decorated : *sorted)
    {
        Row row;
        row.reserve_values(row_width);
        for (size_t i = 0; i < row_width; ++i)
            row.append_value(std::move(decorated.values_[i]));
        std::string key = row_key(row);
        if (have_previous && key == previous)
            continue;
        previous = std::move(key);
        have_previous = true;
        out.rows.push_back(std::move(row));
        if (!aggregate_exprs.empty())
        {
            std::unordered_map<const Expr *, Value> aggs;
            for (size_t i = 0; i < aggregate_exprs.size(); ++i)
                aggs.emplace(aggregate_exprs[i], std::move(decorated.values_[row_width + i]));
            out.row_aggs.push_back(std::move(aggs));
        }
    }
    return out;
}

popo::base::Result<Executor::Batch>
Executor::exec_set_op(const popo::planner::SetOpNode & node, const EvalEnv * outer)
{
    POPO_ASSIGN_OR_RETURN(auto left, exec_batch(node.left, outer));
    POPO_ASSIGN_OR_RETURN(auto right, exec_batch(node.right, outer));
    if (left.width() != right.width())
    {
        return executor_error("set operation branches must have the same number of columns");
    }
    Batch out;
    out.schemas = left.schemas;

    auto counts = [](const std::vector<Row> & rows)
    {
        std::unordered_map<std::string, int> n;
        for (const auto & row : rows)
        {
            n[row_key(row)]++;
        }
        return n;
    };

    if (node.kind == popo::parser::SetOpKind::Union)
    {
        out.rows = std::move(left.rows);
        out.rows.insert(out.rows.end(), std::make_move_iterator(right.rows.begin()), std::make_move_iterator(right.rows.end()));
        if (!node.is_all)
        {
            std::set<std::string> seen;
            std::vector<Row> deduped;
            for (auto & row : out.rows)
            {
                if (seen.insert(row_key(row)).second)
                {
                    deduped.push_back(std::move(row));
                }
            }
            out.rows = std::move(deduped);
        }
        return out;
    }

    auto right_count = counts(right.rows);
    std::unordered_map<std::string, int> emitted;
    for (auto & row : left.rows)
    {
        std::string key = row_key(row);
        int have = right_count[key];
        int used = emitted[key];
        bool keep = false;
        if (node.kind == popo::parser::SetOpKind::Intersect)
        {
            if (node.is_all)
            {
                if (have > 0)
                {
                    right_count[key] = have - 1;
                    keep = true;
                }
            }
            else
            {
                keep = have > 0 && used == 0;
            }
        }
        else if (node.is_all)
        {
            if (have > 0)
            {
                right_count[key] = have - 1;
            }
            else
            {
                keep = true;
            }
        }
        else
        {
            keep = have == 0 && used == 0;
        }
        emitted[key] = used + 1;
        if (keep)
        {
            out.rows.push_back(std::move(row));
        }
    }
    return out;
}

popo::base::Result<Executor::Batch>
Executor::exec_recursive_scan(const popo::planner::RecursiveScanNode & node)
{
    auto it = recursive_inputs_.find(node.cte_name);
    if (it == recursive_inputs_.end())
    {
        return {};
    }
    return it->second;
}

popo::base::Result<Executor::Batch>
Executor::exec_recursive(const popo::planner::SubqueryScanNode & node)
{
    const auto * setop = dynamic_cast<const popo::planner::SetOpNode *>(node.child.get());
    if (!setop || setop->kind != popo::parser::SetOpKind::Union || !setop->is_all)
    {
        return executor_error("WITH RECURSIVE requires a UNION ALL body");
    }
    POPO_ASSIGN_OR_RETURN(auto anchor, exec_batch(setop->left, nullptr));
    Batch working = anchor;
    Batch delta = anchor;
    auto anchor_names = anchor.flat_names();
    for (int step = 0; step < 1000; ++step)
    {
        recursive_inputs_[node.cte_name] = delta;
        POPO_ASSIGN_OR_RETURN(auto next, exec_batch(setop->right, nullptr));
        if (next.rows.empty())
        {
            break;
        }
        if (step == 999)
        {
            recursive_inputs_.erase(node.cte_name);
            return executor_error("WITH RECURSIVE exceeded 1000 iterations");
        }
        // The recursive term's projection is unnamed. Expose the anchor column names
        // so the next iteration resolves the same CTE columns.
        if (next.flat_names().size() == anchor_names.size())
        {
            size_t cursor = 0;
            for (auto & schema : next.schemas)
            {
                for (auto & col : schema.columns)
                {
                    col = anchor_names[cursor++];
                }
            }
        }
        delta.schemas = next.schemas;
        delta.rows = next.rows;
        working.rows.insert(working.rows.end(), next.rows.begin(), next.rows.end());
    }
    recursive_inputs_.erase(node.cte_name);
    return working;
}

popo::base::Result<Executor::Batch>
Executor::exec_window(const popo::planner::WindowNode & node, const EvalEnv * outer)
{
    POPO_ASSIGN_OR_RETURN(auto batch, exec_batch(node.child, outer));
    if (batch.row_aggs.size() < batch.rows.size())
    {
        batch.row_aggs.resize(batch.rows.size());
    }
    EvalEnv env;
    env.layout = &batch;
    env.outer = outer;
    env.depth = outer ? outer->depth + 1 : 0;

    for (const auto & wexpr : node.windows)
    {
        const auto & win = static_cast<const popo::parser::WindowExpr &>(*wexpr);
        std::unordered_map<std::string, std::vector<size_t>> parts;
        std::vector<std::vector<Value>> order_keys(batch.rows.size());
        for (size_t i = 0; i < batch.rows.size(); ++i)
        {
            env.row = &batch.rows[i];
            std::string key;
            for (const auto & part : win.partition_by)
            {
                POPO_ASSIGN_OR_RETURN(auto part_value, eval(part, env));
                key += value_key(part_value);
                key += '\x1f';
            }
            for (const auto & item : win.order_by)
            {
                POPO_ASSIGN_OR_RETURN(auto order_value, eval(item.expr, env));
                order_keys[i].push_back(std::move(order_value));
            }
            parts[key].push_back(i);
        }
        auto peer = [&](size_t a, size_t b)
        {
            for (size_t k = 0; k < win.order_by.size(); ++k)
            {
                if (!sql_eq(order_keys[a][k], order_keys[b][k]))
                    return false;
            }
            return true;
        };
        std::string fname;
        const popo::parser::FunctionCallExpr * agg = nullptr;
        if (win.function && win.function->type == ExprType::ScalarCall)
        {
            fname = static_cast<const popo::parser::ScalarCallExpr &>(*win.function).name;
        }
        else if (win.function && win.function->type == ExprType::FunctionCall)
        {
            agg = &static_cast<const popo::parser::FunctionCallExpr &>(*win.function);
        }
        for (auto & [_, idxs] : parts)
        {
            std::stable_sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b)
            {
                for (size_t k = 0; k < win.order_by.size(); ++k)
                {
                    const auto & item = win.order_by[k];
                    const auto & va = order_keys[a][k];
                    const auto & vb = order_keys[b][k];
                    if (sql_lt(va, vb))
                        return item.ascending;
                    if (sql_lt(vb, va))
                        return !item.ascending;
                }
                return false;
            });
            if (fname == "ROW_NUMBER" || fname == "RANK" || fname == "DENSE_RANK")
            {
                int dense = 0;
                int64_t rank_value = 0;
                for (size_t i = 0; i < idxs.size(); ++i)
                {
                    bool new_peer = i == 0 || !peer(idxs[i], idxs[i - 1]);
                    if (new_peer)
                    {
                        ++dense;
                        rank_value = static_cast<int64_t>(i + 1);
                    }
                    int64_t value = fname == "ROW_NUMBER" ? static_cast<int64_t>(i + 1) : fname == "RANK" ? rank_value : dense;
                    batch.row_aggs[idxs[i]][wexpr.get()] = Value(ValueType::Int64, value);
                }
            }
            else if (agg)
            {
                auto accumulate =
                    [&](size_t index, Value & acc, bool & init, int64_t & count) -> popo::base::Result<void>
                {
                    env.row = &batch.rows[index];
                    bool is_star = agg->args.empty() || agg->args[0]->type == ExprType::Star;
                    Value val(ValueType::Int32, 1);
                    if (!is_star)
                    {
                        POPO_ASSIGN_OR_RETURN(val, eval(agg->args[0], env));
                    }
                    if (!is_star && is_null(val))
                        return {};
                    if (agg->func == popo::parser::AggFuncType::Count)
                    {
                        ++count;
                        return {};
                    }
                    if (agg->func == popo::parser::AggFuncType::Avg || agg->func == popo::parser::AggFuncType::Sum)
                    {
                        if (init)
                        {
                            POPO_ASSIGN_OR_RETURN(acc, do_arithmetic('+', acc, val));
                        }
                        else
                            acc = val;
                        init = true;
                        ++count;
                        return {};
                    }
                    if (agg->func == popo::parser::AggFuncType::Min)
                    {
                        if (!init || sql_lt(val, acc))
                            acc = val;
                        init = true;
                        return {};
                    }
                    if (agg->func == popo::parser::AggFuncType::Max)
                    {
                        if (!init || sql_lt(acc, val))
                            acc = val;
                        init = true;
                    }
                    return {};
                };
                auto finish = [&](const Value & acc, bool init, int64_t count)
                {
                    if (agg->func == popo::parser::AggFuncType::Count)
                        return Value(ValueType::Int64, count);
                    if (agg->func == popo::parser::AggFuncType::Avg)
                    {
                        if (count == 0)
                            return Value();
                        return Value(ValueType::Float64, acc.to_f64() / static_cast<double>(count));
                    }
                    return init ? acc : Value();
                };
                if (win.order_by.empty())
                {
                    Value acc;
                    bool init = false;
                    int64_t count = 0;
                    for (size_t index : idxs)
                    {
                        POPO_TRY(accumulate(index, acc, init, count));
                    }
                    Value value = finish(acc, init, count);
                    for (size_t index : idxs)
                    {
                        batch.row_aggs[index][wexpr.get()] = value;
                    }
                }
                else
                {
                    Value acc;
                    bool init = false;
                    int64_t count = 0;
                    for (size_t index : idxs)
                    {
                        POPO_TRY(accumulate(index, acc, init, count));
                        batch.row_aggs[index][wexpr.get()] = finish(acc, init, count);
                    }
                }
            }
        }
    }
    return batch;
}

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
                bool table_match = table.empty() || eq_ci(table, s.table) || (!s.alias.empty() && eq_ci(table, s.alias));
                if (table_match)
                {
                    for (size_t i = 0; i < s.columns.size(); ++i)
                    {
                        if (eq_ci(s.columns[i], column))
                        {
                            min_env_depth_seen_ = std::min(min_env_depth_seen_, scope->depth);
                            size_t idx = offset + i;
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

popo::base::Result<popo::planner::PlanNodePtr>
Executor::subquery_plan(const popo::parser::Expr & expr, const popo::parser::StmtPtr & query)
{
    auto it = subquery_plans_.find(&expr);
    if (it == subquery_plans_.end())
    {
        auto planned = planner_.plan(query);
        if (!planned)
        {
            return std::unexpected(planned.error());
        }
        it = subquery_plans_.emplace(&expr, optimize_plan(*planned, false)).first;
    }
    return it->second;
}

popo::base::Result<Executor::CachedSubquery *> Executor::eval_subquery_cached(
    const popo::parser::Expr & expr, const popo::parser::StmtPtr & query, const EvalEnv & env, Batch & scratch)
{
    auto hit = uncorrelated_subqueries_.find(&expr);
    if (hit != uncorrelated_subqueries_.end())
    {
        return &hit->second;
    }
    auto correlated_hit = correlated_subqueries_.find(&expr);
    if (correlated_hit != correlated_subqueries_.end()
        && !correlated_hit->second.outer_columns.empty())
    {
        std::string key;
        for (const auto & [table, column] : correlated_hit->second.outer_columns)
        {
            POPO_ASSIGN_OR_RETURN(Value value, resolve_column(table, column, env));
            key += value_key(value);
            key.push_back('\x1f');
        }
        auto entry = correlated_hit->second.entries.find(key);
        if (entry != correlated_hit->second.entries.end())
            return &entry->second;
    }
    int saved = min_env_depth_seen_;
    min_env_depth_seen_ = std::numeric_limits<int>::max();
    std::vector<std::tuple<std::string, std::string, Value>> captured;
    auto * saved_capture = correlation_capture_;
    int saved_capture_depth = correlation_capture_max_depth_;
    correlation_capture_ = &captured;
    correlation_capture_max_depth_ = env.depth;
    auto planned = subquery_plan(expr, query);
    if (!planned)
    {
        correlation_capture_ = saved_capture;
        correlation_capture_max_depth_ = saved_capture_depth;
        return std::unexpected(planned.error());
    }
    auto plan = std::move(*planned);
    auto executed = exec_batch(plan, &env);
    correlation_capture_ = saved_capture;
    correlation_capture_max_depth_ = saved_capture_depth;
    if (!executed)
        return std::unexpected(executed.error());
    scratch = std::move(*executed);
    bool correlated = min_env_depth_seen_ <= env.depth;
    min_env_depth_seen_ = std::min(saved, min_env_depth_seen_);
    if (!correlated)
    {
        CachedSubquery entry;
        entry.batch = std::move(scratch);
        auto [it, _] = uncorrelated_subqueries_.emplace(&expr, std::move(entry));
        return &it->second;
    }
    if (correlated && !captured.empty())
    {
        auto & cache = correlated_subqueries_[&expr];
        if (cache.outer_columns.empty())
        {
            std::set<std::pair<std::string, std::string>> seen;
            for (const auto & [table, column, _] : captured)
                if (seen.emplace(table, column).second)
                    cache.outer_columns.emplace_back(table, column);
        }
        std::string key;
        for (const auto & [table, column] : cache.outer_columns)
        {
            bool found = false;
            for (const auto & [captured_table, captured_column, value] : captured)
            {
                if (captured_table == table && captured_column == column)
                {
                    key += value_key(value);
                    key.push_back('\x1f');
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                POPO_ASSIGN_OR_RETURN(Value value, resolve_column(table, column, env));
                key += value_key(value);
                key.push_back('\x1f');
            }
        }
        size_t bytes = sizeof(CachedSubquery);
        for (const auto & row : scratch.rows)
            bytes += estimate_row_bytes(row);
        if ((!memory_ || memory_->try_reserve(bytes)) && cache.entries.size() < 100000)
        {
            CachedSubquery entry;
            entry.batch = std::move(scratch);
            auto [it, _] = cache.entries.emplace(std::move(key), std::move(entry));
            return &it->second;
        }
    }
    return nullptr;
}

popo::base::Result<Value> Executor::eval_subquery(const popo::parser::Expr & expr, const EvalEnv & env)
{
    switch (expr.type)
    {
        case ExprType::Subquery: {
            const auto & sub = static_cast<const popo::parser::SubqueryExpr &>(expr);
            Batch scratch;
            POPO_ASSIGN_OR_RETURN(auto * cached, eval_subquery_cached(expr, sub.query, env, scratch));
            const Batch & batch = cached ? cached->batch : scratch;
            if (batch.rows.empty())
                return Value();
            if (batch.rows.size() > 1)
                return executor_error("scalar subquery returned more than one row");
            if (batch.rows[0].values_.empty())
                return executor_error("scalar subquery must return one column");
            return batch.rows[0].values_[0];
        }
        case ExprType::InSubquery: {
            const auto & in = static_cast<const popo::parser::InSubqueryExpr &>(expr);
            POPO_ASSIGN_OR_RETURN(Value operand, eval(in.operand, env));
            Batch scratch;
            POPO_ASSIGN_OR_RETURN(auto * cached, eval_subquery_cached(expr, in.query, env, scratch));
            if (cached)
            {
                if (!cached->indexed)
                {
                    cached->first_col_keys.reserve(cached->batch.rows.size());
                    for (const auto & row : cached->batch.rows)
                    {
                        if (row.values_.empty() || is_null(row.values_[0]))
                            cached->has_null = true;
                        else
                            cached->first_col_keys.insert(value_key(row.values_[0]));
                    }
                    cached->indexed = true;
                }
                if (cached->batch.rows.empty())
                {
                    return make_bool(in.negated);
                }
                if (is_null(operand))
                {
                    return make_bool(false);
                }
                bool found = cached->first_col_keys.contains(value_key(operand));
                if (found)
                {
                    return make_bool(!in.negated);
                }
                if (in.negated && cached->has_null)
                {
                    return make_bool(false);
                }
                return make_bool(in.negated);
            }

            const Batch & batch = scratch;
            if (batch.rows.empty())
            {
                return make_bool(in.negated);
            }
            if (is_null(operand))
            {
                return make_bool(false);
            }
            bool found = false;
            bool saw_null = false;
            for (const auto & row : batch.rows)
            {
                if (row.values_.empty() || is_null(row.values_[0]))
                {
                    saw_null = true;
                    continue;
                }
                if (sql_eq(operand, row.values_[0]))
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
        case ExprType::Exists: {
            const auto & ex = static_cast<const popo::parser::ExistsExpr &>(expr);
            Batch scratch;
            POPO_ASSIGN_OR_RETURN(auto * cached, eval_subquery_cached(expr, ex.query, env, scratch));
            const Batch & batch = cached ? cached->batch : scratch;
            return make_bool(!batch.rows.empty());
        }
        default:
            return executor_error("not a subquery expression", popo::base::Errno::InternalErr);
    }
}

std::string trimmed_text(const std::string & text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

popo::base::Result<int64_t> cast_i64(const Value & v)
{
    if (v.datatype() != ValueType::String)
        return to_i64(v);
    std::string text = trimmed_text(v.string_ref());
    if (text.empty())
        return executor_error("CAST from empty string");
    auto integer = popo::base::parse_i64(text);
    if (integer)
        return *integer;
    if (integer.error().error_message.find("overflow") != std::string::npos)
        return executor_error("CAST overflow");
    auto floating = popo::base::parse_f64(text);
    if (!floating)
        return executor_error("CAST could not parse '" + text + "'");
    const long double widened = static_cast<long double>(*floating);
    if (!std::isfinite(*floating)
        || widened < static_cast<long double>(std::numeric_limits<int64_t>::min())
        || widened > static_cast<long double>(std::numeric_limits<int64_t>::max()))
        return executor_error("CAST overflow");
    return static_cast<int64_t>(*floating);
}

popo::base::Result<double> cast_f64(const Value & v)
{
    if (v.datatype() != ValueType::String)
        return v.to_f64();
    std::string text = trimmed_text(v.string_ref());
    auto parsed = popo::base::parse_f64(text);
    if (!parsed)
        return executor_error("CAST could not parse '" + text + "'");
    return *parsed;
}

popo::base::Result<Value> cast_to(const Value & v, ValueType target)
{
    if (is_null(v))
        return Value();
    switch (target)
    {
        case ValueType::Bool:
            return Value(ValueType::Bool, as_bool(v));
        case ValueType::Int8: {
            POPO_ASSIGN_OR_RETURN(auto parsed, cast_i64(v));
            return Value(ValueType::Int8, static_cast<int8_t>(parsed));
        }
        case ValueType::Int16: {
            POPO_ASSIGN_OR_RETURN(auto parsed, cast_i64(v));
            return Value(ValueType::Int16, static_cast<int16_t>(parsed));
        }
        case ValueType::Int32: {
            POPO_ASSIGN_OR_RETURN(auto parsed, cast_i64(v));
            return Value(ValueType::Int32, static_cast<int32_t>(parsed));
        }
        case ValueType::Int64: {
            POPO_ASSIGN_OR_RETURN(auto parsed, cast_i64(v));
            return Value(ValueType::Int64, parsed);
        }
        case ValueType::Float32: {
            POPO_ASSIGN_OR_RETURN(auto parsed, cast_f64(v));
            return Value(ValueType::Float32, static_cast<float>(parsed));
        }
        case ValueType::Float64: {
            POPO_ASSIGN_OR_RETURN(auto parsed, cast_f64(v));
            return Value(ValueType::Float64, parsed);
        }
        case ValueType::String:
            return Value(ValueType::String, v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string());
        default:
            return executor_error("unsupported CAST target");
    }
}

std::string xml_text(const Value & v)
{
    std::string raw = v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string();
    std::string out;
    for (char c : raw)
    {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out.push_back(c);
    }
    return out;
}

bool sql_cmp(popo::parser::BinaryOpType op, const Value & l, const Value & r)
{
    switch (op)
    {
        case popo::parser::BinaryOpType::Eq:
            return sql_eq(l, r);
        case popo::parser::BinaryOpType::Ne:
            return comparable(l, r) && !sql_eq(l, r);
        case popo::parser::BinaryOpType::Lt:
            return sql_lt(l, r);
        case popo::parser::BinaryOpType::Gt:
            return sql_lt(r, l);
        case popo::parser::BinaryOpType::Le:
            return comparable(l, r) && !sql_lt(r, l);
        case popo::parser::BinaryOpType::Ge:
            return comparable(l, r) && !sql_lt(l, r);
        default:
            return false;
    }
}

popo::base::Result<Value>
Executor::eval_quantified(const popo::parser::QuantifiedExpr & expr, const EvalEnv & env)
{
    POPO_ASSIGN_OR_RETURN(Value operand, eval(expr.operand, env));
    Batch scratch;
    POPO_ASSIGN_OR_RETURN(auto * cached, eval_subquery_cached(expr, expr.query, env, scratch));
    const Batch & batch = cached ? cached->batch : scratch;
    if (batch.rows.empty())
    {
        return make_bool(expr.all);
    }
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
    const std::string & name = expr.name;
    auto text_of = [](const Value & v)
    { return v.datatype() == ValueType::String ? std::string(v.string_ref()) : v.to_val_string(); };
    if (name == "COALESCE")
    {
        for (const auto & arg : args)
        {
            if (!is_null(arg))
                return arg;
        }
        return Value();
    }
    if (name == "NULLIF")
    {
        if (args.size() != 2)
            return executor_error("NULLIF expects 2 arguments");
        return sql_eq(args[0], args[1]) ? Value() : args[0];
    }
    if (name == "UPPER" || name == "LOWER")
    {
        if (args.size() != 1)
            return executor_error(name + " expects 1 argument");
        if (is_null(args[0]))
            return Value();
        std::string text = text_of(args[0]);
        for (char & c : text)
        {
            c = static_cast<char>(name == "UPPER" ? std::toupper(static_cast<unsigned char>(c)) : std::tolower(static_cast<unsigned char>(c)));
        }
        return Value(ValueType::String, text);
    }
    if (name == "CHAR_LENGTH" || name == "CHARACTER_LENGTH" || name == "LENGTH")
    {
        if (args.size() != 1)
            return executor_error(name + " expects 1 argument");
        if (is_null(args[0]))
            return Value();
        return Value(ValueType::Int64, static_cast<int64_t>(text_of(args[0]).size()));
    }
    if (name == "SUBSTRING" || name == "SUBSTR")
    {
        if (args.size() < 2 || is_null(args[0]) || is_null(args[1]))
            return Value();
        std::string text = text_of(args[0]);
        int64_t start = to_i64(args[1]);
        if (start < 1)
            start = 1;
        size_t offset = static_cast<size_t>(start - 1);
        if (offset >= text.size())
            return Value(ValueType::String, std::string());
        size_t len = text.size() - offset;
        if (args.size() >= 3 && !is_null(args[2]))
        {
            int64_t asked = to_i64(args[2]);
            if (asked < 0)
                asked = 0;
            len = std::min(len, static_cast<size_t>(asked));
        }
        return Value(ValueType::String, text.substr(offset, len));
    }
    if (name == "TRIM")
    {
        if (args.empty() || is_null(args[0]))
            return Value();
        std::string text = text_of(args[0]);
        std::string mode = "BOTH";
        std::string chars = " ";
        if (args.size() >= 3)
        {
            if (is_null(args[2]))
                return Value();
            mode = text_of(args[1]);
            chars = text_of(args[2]);
        }
        if (chars.empty())
            return Value(ValueType::String, text);
        size_t begin = 0;
        if (mode == "LEADING" || mode == "BOTH")
        {
            while (begin < text.size() && chars.find(text[begin]) != std::string::npos)
                ++begin;
        }
        size_t end = text.size();
        if (mode == "TRAILING" || mode == "BOTH")
        {
            while (end > begin && chars.find(text[end - 1]) != std::string::npos)
                --end;
        }
        return Value(ValueType::String, text.substr(begin, end - begin));
    }
    if (name == "LTRIM" || name == "RTRIM")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        std::string text = text_of(args[0]);
        size_t begin = 0;
        size_t end = text.size();
        if (name == "LTRIM")
        {
            while (begin < end && text[begin] == ' ')
                ++begin;
        }
        else
        {
            while (end > begin && text[end - 1] == ' ')
                --end;
        }
        return Value(ValueType::String, text.substr(begin, end - begin));
    }
    if (name == "EXTRACT")
    {
        if (args.size() != 2 || is_null(args[0]) || is_null(args[1]))
            return Value();
        std::string field = text_of(args[0]);
        for (char & c : field)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        int y = 0, m = 0, d = 0;
        if (!parse_ymd(text_of(args[1]), y, m, d))
            return executor_error("EXTRACT: invalid date '" + text_of(args[1]) + "'");
        if (field == "YEAR")
            return Value(ValueType::Int64, static_cast<int64_t>(y));
        if (field == "MONTH")
            return Value(ValueType::Int64, static_cast<int64_t>(m));
        if (field == "DAY")
            return Value(ValueType::Int64, static_cast<int64_t>(d));
        return executor_error("EXTRACT: unsupported field '" + field + "'");
    }
    if (name == "YEAR" || name == "MONTH" || name == "DAY")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        int y = 0, m = 0, d = 0;
        if (!parse_ymd(text_of(args[0]), y, m, d))
            return executor_error(name + ": invalid date '" + text_of(args[0]) + "'");
        if (name == "YEAR")
            return Value(ValueType::Int64, static_cast<int64_t>(y));
        if (name == "MONTH")
            return Value(ValueType::Int64, static_cast<int64_t>(m));
        return Value(ValueType::Int64, static_cast<int64_t>(d));
    }
    if (name == "ROUND")
    {
        if (args.empty() || is_null(args[0]))
            return Value();
        int64_t scale = (args.size() >= 2 && !is_null(args[1])) ? to_i64(args[1]) : 0;
        double factor = std::pow(10.0, static_cast<double>(scale));
        return Value(ValueType::Float64, std::round(args[0].to_f64() * factor) / factor);
    }
    if (name == "FLOOR")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        return Value(ValueType::Float64, std::floor(args[0].to_f64()));
    }
    if (name == "CEIL" || name == "CEILING")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        return Value(ValueType::Float64, std::ceil(args[0].to_f64()));
    }
    if (name == "MOD")
    {
        if (args.size() != 2 || is_null(args[0]) || is_null(args[1]))
            return Value();
        if (is_floating(args[0]) || is_floating(args[1]))
        {
            double b = args[1].to_f64();
            return b == 0.0 ? Value() : Value(ValueType::Float64, std::fmod(args[0].to_f64(), b));
        }
        int64_t b = to_i64(args[1]);
        return b == 0 ? Value() : Value(ValueType::Int64, to_i64(args[0]) % b);
    }
    if (name == "ABS")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        if (is_floating(args[0]))
            return Value(ValueType::Float64, std::fabs(args[0].to_f64()));
        int64_t n = to_i64(args[0]);
        return Value(ValueType::Int64, n < 0 ? -n : n);
    }
    if (name == "CONCAT")
    {
        std::string text;
        for (const auto & arg : args)
        {
            if (is_null(arg))
                return Value();
            text += text_of(arg);
        }
        return Value(ValueType::String, text);
    }
    if (name == "XMLCOMMENT")
    {
        if (args.size() != 1 || is_null(args[0]))
            return Value();
        return Value(ValueType::String, "<!--" + text_of(args[0]) + "-->");
    }
    if (name == "XMLCONCAT")
    {
        std::string text;
        for (const auto & arg : args)
        {
            if (!is_null(arg))
                text += text_of(arg);
        }
        return Value(ValueType::String, text);
    }
    if (name == "XMLELEMENT")
    {
        if (args.empty())
            return executor_error("XMLELEMENT requires a name");
        std::string tag = text_of(args[0]);
        std::string body = args.size() > 1 && !is_null(args[1]) ? xml_text(args[1]) : "";
        return Value(ValueType::String, "<" + tag + ">" + body + "</" + tag + ">");
    }
    if (name == "XMLFOREST")
    {
        std::string text;
        for (size_t i = 0; i + 1 < args.size(); i += 2)
        {
            if (is_null(args[i + 1]))
                continue;
            std::string tag = text_of(args[i]);
            text += "<" + tag + ">" + xml_text(args[i + 1]) + "</" + tag + ">";
        }
        return Value(ValueType::String, text);
    }
    return executor_error("unknown function " + name);
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
            bool in_range = comparable(v, lo) && comparable(v, hi) && !(v < lo) && !(hi < v);
            return make_bool(b.negated ? !in_range : in_range);
        }
        case ExprType::Like: {
            const auto & like = static_cast<const popo::parser::LikeExpr &>(*expr);
            POPO_ASSIGN_OR_RETURN(Value v, eval(like.operand, env));
            POPO_ASSIGN_OR_RETURN(Value p, eval(like.pattern, env));
            if (is_null(v) || is_null(p))
                return make_bool(false);
            bool matched = like_match(v.to_val_string(), p.to_val_string());
            return make_bool(like.negated ? !matched : matched);
        }
        case ExprType::IsNull: {
            const auto & isnull = static_cast<const popo::parser::IsNullExpr &>(*expr);
            POPO_ASSIGN_OR_RETURN(auto operand, eval(isnull.operand, env));
            bool null = is_null(operand);
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
                bool hit = c.operand ? sql_eq(operand, cv) : as_bool(cv);
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
            bool same = (is_null(l) && is_null(r)) || sql_eq(l, r);
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

popo::base::Result<void> Executor::exec_insert(const popo::planner::InsertNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
    {
        return executor_error(std::format("Table '{}' not found", node.table_name), popo::base::Errno::TableNotFound);
    }
    auto * heap = engine_.get_table_heap(node.table_name);
    if (!heap)
    {
        return executor_error(
            std::format("Table '{}' has no storage", node.table_name), popo::base::Errno::InternalErr);
    }
    const auto & schema = *qr;
    std::vector<int32_t> positions(schema->columns_.size(), -1);
    if (node.columns.empty())
    {
        for (size_t i = 0; i < positions.size(); ++i)
        {
            positions[i] = static_cast<int32_t>(i);
        }
    }
    else
    {
        for (size_t i = 0; i < node.columns.size(); ++i)
        {
            bool found = false;
            for (size_t j = 0; j < schema->columns_.size(); ++j)
            {
                if (eq_ci(schema->columns_[j].column_name_, node.columns[i]))
                {
                    positions[j] = static_cast<int32_t>(i);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return executor_error(std::format("Column '{}' not found", node.columns[i]));
            }
        }
    }

    EvalEnv env;
    for (const auto & values : node.values)
    {
        if (values.size() > (node.columns.empty() ? schema->columns_.size() : node.columns.size()))
        {
            return executor_error("INSERT has more values than columns");
        }
        std::vector<Value> evaluated;
        evaluated.reserve(values.size());
        for (const auto & e : values)
        {
            POPO_ASSIGN_OR_RETURN(auto value, eval(e, env));
            evaluated.push_back(std::move(value));
        }
        Row row;
        row.reserve_values(schema->columns_.size());
        for (size_t j = 0; j < schema->columns_.size(); ++j)
        {
            int32_t pos = positions[j];
            if (pos >= 0 && static_cast<size_t>(pos) < evaluated.size())
            {
                row.append_value(evaluated[static_cast<size_t>(pos)]);
            }
            else if (schema->columns_[j].default_value_.has_value())
            {
                row.append_value(*schema->columns_[j].default_value_);
            }
            else
            {
                row.append_value(Value());
            }
        }
        POPO_TRY(engine_.mvcc()->stage_insert(txn_, node.table_name, row));
        result_.rows_affected++;
    }
    return {};
}

popo::base::Result<void> Executor::exec_update(const popo::planner::UpdateNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
    {
        return executor_error(std::format("Table '{}' not found", node.table_name), popo::base::Errno::TableNotFound);
    }
    auto * heap = engine_.get_table_heap(node.table_name);
    if (!heap)
        return {};
    const auto & schema = *qr;

    Batch layout;
    ColumnSchema cs;
    cs.table = node.table_name;
    for (const auto & col : schema->columns_)
    {
        cs.columns.push_back(col.column_name_);
    }
    layout.schemas.push_back(std::move(cs));

    std::vector<Row> targets;
    {
        EvalEnv env;
        env.layout = &layout;
        for (auto & row : engine_.mvcc()->scan(txn_, node.table_name))
        {
            env.row = &row;
            bool selected = true;
            if (node.where_clause)
            {
                POPO_ASSIGN_OR_RETURN(auto condition, eval(node.where_clause, env));
                selected = as_bool(condition);
            }
            if (selected)
            {
                targets.push_back(std::move(row));
            }
        }
    }
    for (auto & row : targets)
    {
        EvalEnv env;
        env.layout = &layout;
        env.row = &row;
        Row updated = row;
        for (const auto & assign : node.assignments)
        {
            int32_t idx = -1;
            for (size_t j = 0; j < schema->columns_.size(); ++j)
            {
                if (eq_ci(schema->columns_[j].column_name_, assign.column_name))
                {
                    idx = static_cast<int32_t>(j);
                    break;
                }
            }
            if (idx < 0)
            {
                return executor_error(std::format("Column '{}' not found", assign.column_name));
            }
            POPO_ASSIGN_OR_RETURN(updated.values_[static_cast<size_t>(idx)], eval(assign.value, env));
        }
        Value old_pk = schema->extract_pk(row);
        Value new_pk = schema->extract_pk(updated);
        if (!(old_pk == new_pk))
        {
            POPO_TRY(engine_.mvcc()->stage_delete(txn_, node.table_name, old_pk));
            POPO_TRY(engine_.mvcc()->stage_insert(txn_, node.table_name, updated));
        }
        else
        {
            POPO_TRY(engine_.mvcc()->stage_update(txn_, node.table_name, old_pk, updated));
        }
        result_.rows_affected++;
    }
    return {};
}

popo::base::Result<void> Executor::exec_delete(const popo::planner::DeleteNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
    {
        return executor_error(std::format("Table '{}' not found", node.table_name), popo::base::Errno::TableNotFound);
    }
    auto * heap = engine_.get_table_heap(node.table_name);
    if (!heap)
        return {};

    Batch layout;
    ColumnSchema cs;
    cs.table = node.table_name;
    for (const auto & col : (*qr)->columns_)
    {
        cs.columns.push_back(col.column_name_);
    }
    layout.schemas.push_back(std::move(cs));

    std::vector<Value> targets;
    {
        EvalEnv env;
        env.layout = &layout;
        for (auto & row : engine_.mvcc()->scan(txn_, node.table_name))
        {
            env.row = &row;
            bool selected = true;
            if (node.where_clause)
            {
                POPO_ASSIGN_OR_RETURN(auto condition, eval(node.where_clause, env));
                selected = as_bool(condition);
            }
            if (selected)
            {
                targets.push_back((*qr)->extract_pk(row));
            }
        }
    }
    for (auto & pk : targets)
    {
        POPO_TRY(engine_.mvcc()->stage_delete(txn_, node.table_name, pk));
        result_.rows_affected++;
    }
    return {};
}

popo::base::Result<void> Executor::exec_alter_table(const popo::planner::AlterTableNode & node)
{
    auto qr = schema_mgr_.query_table(node.table_name);
    if (!qr || !*qr)
    {
        return executor_error(std::format("Table '{}' not found", node.table_name), popo::base::Errno::TableNotFound);
    }
    if (node.action == popo::parser::AlterAction::NoOp)
    {
        result_.message = "Table altered";
        return {};
    }
    const auto old_schema = **qr;

    std::vector<Row> rows = engine_.mvcc()->scan(txn_, node.table_name);

    auto columns = old_schema.columns_;
    std::string new_name = node.table_name;

    switch (node.action)
    {
        case popo::parser::AlterAction::AddColumn: {
            for (const auto & col : columns)
            {
                if (eq_ci(col.column_name_, node.column.name))
                {
                    return executor_error(std::format("Column '{}' already exists", node.column.name));
                }
            }
            popo::catalog::Column col{};
            col.column_name_ = node.column.name;
            col.data_type_ = node.column.data_type;
            col.type_len_ = node.column.type_len;
            col.is_nullable_ = node.column.is_nullable;
            col.default_value_ = node.column.default_value;
            columns.push_back(col);
            Value fill = node.column.default_value.value_or(Value());
            for (auto & row : rows)
            {
                row.append_value(fill);
            }
            break;
        }
        case popo::parser::AlterAction::DropColumn: {
            int32_t idx = -1;
            for (size_t i = 0; i < columns.size(); ++i)
            {
                if (eq_ci(columns[i].column_name_, node.target_name))
                {
                    idx = static_cast<int32_t>(i);
                    break;
                }
            }
            if (idx < 0)
            {
                return executor_error(std::format("Column '{}' not found", node.target_name));
            }
            if (columns[static_cast<size_t>(idx)].is_primary_key_)
            {
                return executor_error("cannot drop the primary key column");
            }
            if (columns.size() == 1)
            {
                return executor_error("cannot drop the last column");
            }
            columns.erase(columns.begin() + idx);
            for (auto & row : rows)
            {
                row.values_.erase(row.values_.begin() + idx);
            }
            break;
        }
        case popo::parser::AlterAction::RenameTable: {
            auto existing = schema_mgr_.query_table(node.target_name);
            if (existing && *existing)
            {
                return executor_error(
                    std::format("Table '{}' already exists", node.target_name), popo::base::Errno::TableAlreadyExists);
            }
            new_name = node.target_name;
            break;
        }
        case popo::parser::AlterAction::AddPrimaryKey: {
            for (const auto & col : columns)
            {
                if (col.is_primary_key_)
                {
                    return executor_error("table already has a primary key");
                }
            }
            for (const auto & kname : node.key_columns)
            {
                int32_t idx = -1;
                for (size_t i = 0; i < columns.size(); ++i)
                {
                    if (eq_ci(columns[i].column_name_, kname))
                    {
                        idx = static_cast<int32_t>(i);
                        break;
                    }
                }
                if (idx < 0)
                {
                    return executor_error(std::format("Column '{}' not found", kname));
                }
                for (const auto & row : rows)
                {
                    if (static_cast<size_t>(idx) >= row.values_.size() || is_null(row.values_[static_cast<size_t>(idx)]))
                    {
                        return executor_error("primary key column cannot contain NULL");
                    }
                }
                columns[static_cast<size_t>(idx)].is_primary_key_ = true;
                columns[static_cast<size_t>(idx)].is_nullable_ = false;
            }
            break;
        }
        case popo::parser::AlterAction::NoOp:
            break;
    }

    POPO_TRY(rebuild_table(node.table_name, columns, rows, new_name));
    result_.message = "Table altered";
    return {};
}

popo::base::Result<void> Executor::rebuild_table(
    const std::string & table_name, const std::vector<popo::catalog::Column> & new_columns, const std::vector<Row> & rows,
    const std::string & new_name)
{
    auto qr = schema_mgr_.query_table(table_name);
    if (!qr || !*qr)
    {
        return executor_error(std::format("Table '{}' not found", table_name), popo::base::Errno::TableNotFound);
    }
    const auto old_schema = **qr;

    if (new_name != table_name)
    {
        std::vector<popo::catalog::Column> renamed = new_columns;
        auto created = schema_mgr_.create_table(new_name, renamed);
        if (!created)
        {
            return executor_error(
                "failed to create renamed table: " + new_name, popo::base::Errno::TableAlreadyExists);
        }
        auto del = schema_mgr_.delete_table(table_name);
        if (!del)
        {
            return std::unexpected(del.error());
        }
    }
    else
    {
        auto modified = schema_mgr_.modify_table(table_name, new_columns);
        if (!modified)
        {
            return std::unexpected(modified.error());
        }
    }

    engine_.drop_table(table_name);
    auto new_qr = schema_mgr_.query_table(new_name);
    if (!new_qr || !*new_qr)
    {
        return executor_error("schema missing after ALTER TABLE", popo::base::Errno::InternalErr);
    }
    POPO_TRY(engine_.create_table(new_name, **new_qr));
    auto * heap = engine_.get_table_heap(new_name);
    if (!heap)
    {
        return executor_error("storage missing after ALTER TABLE", popo::base::Errno::InternalErr);
    }

    // Recreate secondary indexes whose columns survived, remapping column positions
    for (const auto & index : old_schema.indexes_)
    {
        std::vector<int32_t> mapped;
        bool valid = true;
        for (int32_t old_idx : index.index_cols_)
        {
            int32_t new_idx = -1;
            for (size_t j = 0; j < new_columns.size(); ++j)
            {
                if (old_idx >= 0 && static_cast<size_t>(old_idx) < old_schema.columns_.size()
                    && eq_ci(new_columns[j].column_name_, old_schema.columns_[static_cast<size_t>(old_idx)].column_name_))
                {
                    new_idx = static_cast<int32_t>(j);
                    break;
                }
            }
            if (new_idx < 0)
            {
                valid = false;
                break;
            }
            mapped.push_back(new_idx);
        }
        if (new_name == table_name)
        {
            auto del = schema_mgr_.delete_index(table_name, index.index_name_);
            if (!del)
            {
                return std::unexpected(del.error());
            }
        }
        if (!valid || mapped.size() != 1)
            continue;
        auto created = schema_mgr_.create_index(new_name, index.index_name_, mapped, index.is_unique_);
        if (!created)
        {
            return std::unexpected(created.error());
        }
        POPO_TRY(engine_.create_index(new_name, index.index_name_, mapped[0], index.is_unique_));
    }

    for (const auto & row : rows)
    {
        POPO_TRY(engine_.mvcc()->stage_insert(txn_, new_name, row));
    }
    return {};
}

popo::base::Result<void> Executor::exec_create_view(const popo::planner::CreateViewNode & node)
{
    auto created = schema_mgr_.create_view(node.name, node.query, node.column_names, node.or_replace);
    if (!created)
    {
        return std::unexpected(created.error());
    }
    result_.message = "View created";
    return {};
}

popo::base::Result<void> Executor::exec_drop_view(const popo::planner::DropViewNode & node)
{
    auto dropped = schema_mgr_.drop_view(node.name, node.if_exists);
    if (!dropped)
    {
        return std::unexpected(dropped.error());
    }
    result_.message = "View dropped";
    return {};
}

} // namespace popo::executor