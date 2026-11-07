#include "parser/parser.h"

#include <cctype>
#include <format>
#include <memory>
#include <stdexcept>

namespace popo::parser
{

namespace
{
struct ParseError
{
    std::string msg;
    explicit ParseError(std::string m) : msg(std::move(m)) { }
};
} // namespace

// Token access helpers

const Token & Parser::current() const
{
    if (pos_ >= tokens_.size())
    {
        static const Token eof{TokenType::Eof, "", -1, -1};
        return eof;
    }
    return tokens_[pos_];
}

const Token & Parser::peek_token() const
{
    if (pos_ + 1 >= tokens_.size())
    {
        static const Token eof{TokenType::Eof, "", -1, -1};
        return eof;
    }
    return tokens_[pos_ + 1];
}

const Token & Parser::advance_token()
{
    if (pos_ >= tokens_.size())
    {
        static const Token eof{TokenType::Eof, "", -1, -1};
        return eof;
    }
    return tokens_[pos_++];
}

bool Parser::check(TokenType type) const
{
    return current().type == type;
}

bool Parser::match(TokenType type)
{
    if (check(type))
    {
        advance_token();
        return true;
    }
    return false;
}

Token Parser::expect(TokenType type)
{
    if (!check(type))
    {
        throw ParseError(std::format("Expected token type {}, got '{}'", static_cast<int>(type), current().value));
    }
    return advance_token();
}

bool Parser::is_at_end() const
{
    return check(TokenType::Eof);
}

// Main parse entry point

popo::base::Result<std::vector<StmtPtr>> Parser::parse(const std::string & sql)
{
    try
    {
        Lexer lexer(sql);
        tokens_ = lexer.tokenize();
        pos_ = 0;

        std::vector<StmtPtr> statements;
        while (!is_at_end())
        {
            if (match(TokenType::Semicolon))
            {
                continue;
            }
            auto stmt = parse_statement();
            if (stmt)
            {
                statements.push_back(std::move(stmt));
            }
            match(TokenType::Semicolon);
        }
        return statements;
    }
    catch (const ParseError & e)
    {
        return std::unexpected(base::Error(static_cast<int32_t>(base::Errno::InvalidArg), e.msg));
    }
}

// Statement parsing

StmtPtr Parser::parse_statement()
{
    if (match(TokenType::With))
    {
        bool recursive = match(TokenType::Recursive);
        auto ctes = parse_with_clause();
        for (auto & cte : ctes)
        {
            cte.recursive = recursive;
        }
        expect(TokenType::Select);
        auto stmt = std::dynamic_pointer_cast<SelectStatement>(parse_select());
        stmt->with_clause = std::move(ctes);
        return stmt;
    }
    if (match(TokenType::Select))
    {
        // Check for SELECT DATABASE()
        if (current().type == TokenType::Identifier && (current().value == "database" || current().value == "DATABASE")
            && peek_token().type == TokenType::LParen)
        {
            return parse_select_database();
        }
        // Check for SELECT @@xxx
        if (current().type == TokenType::SystemVar)
        {
            return parse_select_system_var();
        }
        return parse_select();
    }
    if (match(TokenType::Insert))
        return parse_insert();
    if (match(TokenType::Create))
    {
        if (check(TokenType::Unique))
        {
            advance_token();
            return parse_create_index(true);
        }
        if (check(TokenType::Index))
        {
            return parse_create_index(false);
        }
        if (match(TokenType::View))
        {
            return parse_create_view();
        }
        return parse_create_table();
    }
    if (match(TokenType::Update))
        return parse_update();
    if (match(TokenType::Delete))
        return parse_delete();
    if (match(TokenType::Drop))
        return parse_drop();
    if (match(TokenType::Alter))
        return parse_alter_table();
    if (match(TokenType::Show))
        return parse_show();
    if (match(TokenType::Desc) || match(TokenType::Describe))
        return parse_describe();
    if (match(TokenType::Set))
        return parse_set();
    throw ParseError(std::format("Unexpected token '{}', expected statement", current().value));
}

StmtPtr Parser::parse_select()
{
    auto stmt = std::make_shared<SelectStatement>();
    parse_select_core(stmt);

    while (check(TokenType::Union) || check(TokenType::Intersect) || check(TokenType::Except))
    {
        SetTerm term;
        if (match(TokenType::Intersect))
        {
            term.kind = SetOpKind::Intersect;
        }
        else if (match(TokenType::Except))
        {
            term.kind = SetOpKind::Except;
        }
        else
        {
            expect(TokenType::Union);
            term.kind = SetOpKind::Union;
        }
        term.is_all = match(TokenType::All);
        expect(TokenType::Select);
        term.query = std::make_shared<SelectStatement>();
        parse_select_core(term.query);
        stmt->set_ops.push_back(std::move(term));
    }

    if (match(TokenType::Order))
    {
        expect(TokenType::By);
        stmt->order_by = parse_order_by();
    }

    if (match(TokenType::Limit))
    {
        stmt->limit = parse_limit();
    }

    if (match(TokenType::Offset))
    {
        stmt->offset = parse_limit();
    }

    return stmt;
}

void Parser::parse_select_core(const std::shared_ptr<SelectStatement> & stmt)
{
    if (match(TokenType::Distinct))
    {
        stmt->distinct = true;
    }
    else
    {
        match(TokenType::All);
    }
    stmt->select_list = parse_select_list();

    if (match(TokenType::From))
    {
        stmt->from = parse_table_ref();
    }

    if (match(TokenType::Where))
    {
        stmt->where = parse_expr();
    }

    if (match(TokenType::Group))
    {
        expect(TokenType::By);
        stmt->group_by = parse_group_by();
    }

    if (match(TokenType::Having))
    {
        stmt->having = parse_expr();
    }
}

std::vector<CteDef> Parser::parse_with_clause()
{
    std::vector<CteDef> ctes;
    do
    {
        CteDef cte;
        cte.name = expect(TokenType::Identifier).value;
        expect(TokenType::As);
        expect(TokenType::LParen);
        expect(TokenType::Select);
        cte.query = std::dynamic_pointer_cast<SelectStatement>(parse_select());
        expect(TokenType::RParen);
        ctes.push_back(std::move(cte));
    } while (match(TokenType::Comma));
    return ctes;
}

StmtPtr Parser::parse_alter_table()
{
    expect(TokenType::Table);
    auto stmt = std::make_shared<AlterTableStatement>();
    stmt->table_name = expect(TokenType::Identifier).value;

    if (match(TokenType::Add))
    {
        match(TokenType::Column);
        stmt->action = AlterAction::AddColumn;
        stmt->column = parse_column_def();
    }
    else if (match(TokenType::Drop))
    {
        match(TokenType::Column);
        stmt->action = AlterAction::DropColumn;
        stmt->target_name = expect(TokenType::Identifier).value;
    }
    else if (match(TokenType::Rename))
    {
        expect(TokenType::To);
        stmt->action = AlterAction::RenameTable;
        stmt->target_name = expect(TokenType::Identifier).value;
    }
    else
    {
        throw ParseError(std::format("Unsupported ALTER TABLE action near '{}'", current().value));
    }
    return stmt;
}

StmtPtr Parser::parse_insert()
{
    expect(TokenType::Into);
    Token table = expect(TokenType::Identifier);

    auto stmt = std::make_shared<InsertStatement>();
    stmt->table_name = table.value;

    if (match(TokenType::LParen))
    {
        do
        {
            Token col = expect(TokenType::Identifier);
            stmt->columns.push_back(col.value);
        } while (match(TokenType::Comma));
        expect(TokenType::RParen);
    }

    expect(TokenType::Values);

    do
    {
        expect(TokenType::LParen);
        std::vector<ExprPtr> row;
        do
        {
            row.push_back(parse_expr());
        } while (match(TokenType::Comma));
        expect(TokenType::RParen);
        stmt->values.push_back(std::move(row));
    } while (match(TokenType::Comma));

    return stmt;
}

StmtPtr Parser::parse_create_table()
{
    expect(TokenType::Table);
    Token table = expect(TokenType::Identifier);

    auto stmt = std::make_shared<CreateTableStatement>();
    stmt->table_name = table.value;

    expect(TokenType::LParen);
    do
    {
        stmt->columns.push_back(parse_column_def());
    } while (match(TokenType::Comma));
    expect(TokenType::RParen);

    return stmt;
}

StmtPtr Parser::parse_create_index(bool unique)
{
    expect(TokenType::Index);
    Token name = expect(TokenType::Identifier);
    expect(TokenType::On);
    Token table = expect(TokenType::Identifier);
    expect(TokenType::LParen);
    auto stmt = std::make_shared<CreateIndexStatement>();
    stmt->unique = unique;
    stmt->index_name = name.value;
    stmt->table_name = table.value;
    do
    {
        stmt->columns.push_back(expect(TokenType::Identifier).value);
    } while (match(TokenType::Comma));
    expect(TokenType::RParen);
    if (stmt->columns.size() != 1)
    {
        throw ParseError("only a single-column secondary index is supported");
    }
    return stmt;
}

StmtPtr Parser::parse_update()
{
    Token table = expect(TokenType::Identifier);

    auto stmt = std::make_shared<UpdateStatement>();
    stmt->table_name = table.value;

    expect(TokenType::Set);

    do
    {
        Token col = expect(TokenType::Identifier);
        expect(TokenType::Eq);
        UpdateAssignment assign;
        assign.column_name = col.value;
        assign.value = parse_expr();
        stmt->assignments.push_back(std::move(assign));
    } while (match(TokenType::Comma));

    if (match(TokenType::Where))
    {
        stmt->where = parse_expr();
    }

    return stmt;
}

StmtPtr Parser::parse_delete()
{
    expect(TokenType::From);
    Token table = expect(TokenType::Identifier);

    auto stmt = std::make_shared<DeleteStatement>();
    stmt->table_name = table.value;

    if (match(TokenType::Where))
    {
        stmt->where = parse_expr();
    }

    return stmt;
}

StmtPtr Parser::parse_create_view()
{
    auto stmt = std::make_shared<CreateViewStatement>();
    stmt->name = expect(TokenType::Identifier).value;
    expect(TokenType::As);
    expect(TokenType::Select);
    stmt->query = parse_select();
    return stmt;
}

StmtPtr Parser::parse_drop()
{
    if (match(TokenType::View))
    {
        auto stmt = std::make_shared<DropViewStatement>();
        stmt->name = expect(TokenType::Identifier).value;
        return stmt;
    }
    expect(TokenType::Table);

    auto stmt = std::make_shared<DropTableStatement>();

    if (match(TokenType::If))
    {
        expect(TokenType::Exists);
        stmt->if_exists = true;
    }

    Token table = expect(TokenType::Identifier);
    stmt->table_name = table.value;

    return stmt;
}

// SELECT helpers

std::vector<ExprPtr> Parser::parse_select_list()
{
    auto parse_item = [&]() -> ExprPtr
    {
        auto expr = parse_expr();
        std::string alias;
        if (match(TokenType::As))
        {
            alias = expect(TokenType::Identifier).value;
        }
        else if (check(TokenType::Identifier))
        {
            alias = advance_token().value;
        }
        if (!alias.empty())
        {
            auto wrapped = std::make_shared<AliasExpr>();
            wrapped->expr = expr;
            wrapped->alias = alias;
            return wrapped;
        }
        return expr;
    };
    std::vector<ExprPtr> list;
    list.push_back(parse_item());
    while (match(TokenType::Comma))
    {
        list.push_back(parse_item());
    }
    return list;
}

TableRefPtr Parser::parse_table_primary()
{
    if (match(TokenType::LParen))
    {
        expect(TokenType::Select);
        auto ref = std::make_shared<DerivedTableRef>();
        ref->query = parse_select();
        expect(TokenType::RParen);
        if (match(TokenType::As))
        {
            ref->alias = expect(TokenType::Identifier).value;
        }
        else if (check(TokenType::Identifier))
        {
            ref->alias = advance_token().value;
        }
        return ref;
    }
    Token name = expect(TokenType::Identifier);
    auto ref = std::make_shared<SimpleTableRef>();
    ref->table_name = name.value;
    if (match(TokenType::As))
    {
        ref->alias = expect(TokenType::Identifier).value;
    }
    else if (check(TokenType::Identifier))
    {
        ref->alias = advance_token().value;
    }
    return ref;
}

TableRefPtr Parser::parse_table_ref()
{
    auto left = parse_table_primary();

    while (true)
    {
        if (check(TokenType::Comma))
        {
            advance_token();
            auto right = parse_table_primary();
            auto join = std::make_shared<JoinTableRef>();
            join->join_type = JoinType::Cross;
            join->left = left;
            join->right = right;
            left = join;
        }
        else if (
            check(TokenType::Join) || check(TokenType::Inner) || check(TokenType::Left) || check(TokenType::Right)
            || check(TokenType::Full) || check(TokenType::Cross))
        {
            left = parse_join(left);
        }
        else
        {
            break;
        }
    }

    return left;
}

TableRefPtr Parser::parse_join(TableRefPtr left)
{
    JoinType join_type = JoinType::Inner;

    if (check(TokenType::Inner))
    {
        advance_token();
    }
    else if (check(TokenType::Left))
    {
        advance_token();
        join_type = JoinType::Left;
        match(TokenType::Outer);
    }
    else if (check(TokenType::Right))
    {
        advance_token();
        join_type = JoinType::Right;
        match(TokenType::Outer);
    }
    else if (check(TokenType::Full))
    {
        advance_token();
        join_type = JoinType::Full;
        match(TokenType::Outer);
    }
    else if (check(TokenType::Cross))
    {
        advance_token();
        join_type = JoinType::Cross;
    }

    expect(TokenType::Join);

    auto right = parse_table_primary();

    ExprPtr condition = nullptr;
    if (join_type != JoinType::Cross)
    {
        if (match(TokenType::On))
        {
            condition = parse_expr();
        }
    }

    auto join = std::make_shared<JoinTableRef>();
    join->join_type = join_type;
    join->left = left;
    join->right = right;
    join->condition = condition;
    return join;
}

ExprPtr Parser::parse_where()
{
    return parse_expr();
}

std::vector<ExprPtr> Parser::parse_group_by()
{
    std::vector<ExprPtr> list;
    list.push_back(parse_expr());
    while (match(TokenType::Comma))
    {
        list.push_back(parse_expr());
    }
    return list;
}

ExprPtr Parser::parse_having()
{
    return parse_expr();
}

std::vector<OrderByItem> Parser::parse_order_by()
{
    std::vector<OrderByItem> list;
    do
    {
        OrderByItem item;
        item.expr = parse_expr();
        if (match(TokenType::Asc))
        {
            item.ascending = true;
        }
        else if (match(TokenType::Desc))
        {
            item.ascending = false;
        }
        list.push_back(std::move(item));
    } while (match(TokenType::Comma));
    return list;
}

std::optional<int64_t> Parser::parse_limit()
{
    Token tok = expect(TokenType::Integer);
    return std::stoll(tok.value);
}

// Expression parsing (precedence climbing)

ExprPtr Parser::parse_expr()
{
    return parse_or_expr();
}

ExprPtr Parser::parse_or_expr()
{
    auto left = parse_and_expr();
    while (match(TokenType::Or))
    {
        auto binary = std::make_shared<BinaryOpExpr>();
        binary->op = BinaryOpType::Or;
        binary->left = left;
        binary->right = parse_and_expr();
        left = binary;
    }
    return left;
}

ExprPtr Parser::parse_and_expr()
{
    auto left = parse_not_expr();
    while (match(TokenType::And))
    {
        auto binary = std::make_shared<BinaryOpExpr>();
        binary->op = BinaryOpType::And;
        binary->left = left;
        binary->right = parse_not_expr();
        left = binary;
    }
    return left;
}

ExprPtr Parser::parse_not_expr()
{
    if (match(TokenType::Not))
    {
        auto unary = std::make_shared<UnaryOpExpr>();
        unary->op = UnaryOpType::Not;
        unary->operand = parse_not_expr();
        return unary;
    }
    return parse_comparison();
}

ExprPtr Parser::parse_comparison()
{
    auto left = parse_addition();
    while (true)
    {
        bool negated = false;
        if (check(TokenType::Not)
            && (peek_token().type == TokenType::In || peek_token().type == TokenType::Between || peek_token().type == TokenType::Like))
        {
            advance_token();
            negated = true;
        }
        if (negated || check(TokenType::In) || check(TokenType::Between) || check(TokenType::Like))
        {
            if (match(TokenType::In))
            {
                expect(TokenType::LParen);
                if (check(TokenType::Select))
                {
                    advance_token();
                    auto in_sub = std::make_shared<InSubqueryExpr>();
                    in_sub->operand = left;
                    in_sub->query = parse_select();
                    in_sub->negated = negated;
                    expect(TokenType::RParen);
                    left = in_sub;
                }
                else
                {
                    auto in = std::make_shared<InExpr>();
                    in->operand = left;
                    in->negated = negated;
                    do
                    {
                        in->list.push_back(parse_expr());
                    } while (match(TokenType::Comma));
                    expect(TokenType::RParen);
                    left = in;
                }
            }
            else if (match(TokenType::Between))
            {
                auto between = std::make_shared<BetweenExpr>();
                between->operand = left;
                between->negated = negated;
                between->low = parse_addition();
                expect(TokenType::And);
                between->high = parse_addition();
                left = between;
            }
            else if (match(TokenType::Like))
            {
                auto like = std::make_shared<LikeExpr>();
                like->operand = left;
                like->negated = negated;
                like->pattern = parse_addition();
                left = like;
            }
            continue;
        }
        if (match(TokenType::Is))
        {
            bool negated = match(TokenType::Not);
            if (match(TokenType::Distinct))
            {
                expect(TokenType::From);
                auto distinct = std::make_shared<DistinctFromExpr>();
                distinct->left = left;
                distinct->negated = negated;
                distinct->right = parse_addition();
                left = distinct;
                continue;
            }
            auto is_null = std::make_shared<IsNullExpr>();
            is_null->operand = left;
            is_null->negated = negated;
            expect(TokenType::Null);
            left = is_null;
            continue;
        }
        if (match(TokenType::Eq))
        {
            left = parse_comparison_rhs(BinaryOpType::Eq, left);
        }
        else if (match(TokenType::Ne))
        {
            left = parse_comparison_rhs(BinaryOpType::Ne, left);
        }
        else if (match(TokenType::Lt))
        {
            left = parse_comparison_rhs(BinaryOpType::Lt, left);
        }
        else if (match(TokenType::Le))
        {
            left = parse_comparison_rhs(BinaryOpType::Le, left);
        }
        else if (match(TokenType::Gt))
        {
            left = parse_comparison_rhs(BinaryOpType::Gt, left);
        }
        else if (match(TokenType::Ge))
        {
            left = parse_comparison_rhs(BinaryOpType::Ge, left);
        }
        else
        {
            break;
        }
    }
    return left;
}

ExprPtr Parser::parse_addition()
{
    auto left = parse_multiplication();
    while (true)
    {
        if (match(TokenType::Plus))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Add;
            binary->left = left;
            binary->right = parse_multiplication();
            left = binary;
        }
        else if (match(TokenType::Minus))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Sub;
            binary->left = left;
            binary->right = parse_multiplication();
            left = binary;
        }
        else if (match(TokenType::Concat))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Concat;
            binary->left = left;
            binary->right = parse_multiplication();
            left = binary;
        }
        else
        {
            break;
        }
    }
    return left;
}

ExprPtr Parser::parse_multiplication()
{
    auto left = parse_unary();
    while (true)
    {
        if (match(TokenType::Star))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Mul;
            binary->left = left;
            binary->right = parse_unary();
            left = binary;
        }
        else if (match(TokenType::Slash))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Div;
            binary->left = left;
            binary->right = parse_unary();
            left = binary;
        }
        else
        {
            break;
        }
    }
    return left;
}

ExprPtr Parser::parse_unary()
{
    if (match(TokenType::Minus))
    {
        auto unary = std::make_shared<UnaryOpExpr>();
        unary->op = UnaryOpType::Neg;
        unary->operand = parse_unary();
        return unary;
    }
    if (match(TokenType::Not))
    {
        auto unary = std::make_shared<UnaryOpExpr>();
        unary->op = UnaryOpType::Not;
        unary->operand = parse_unary();
        return unary;
    }
    return parse_primary();
}

ExprPtr Parser::parse_primary()
{
    if (check(TokenType::Integer))
    {
        Token tok = advance_token();
        auto lit = std::make_shared<LiteralExpr>();
        lit->value = type::Value(type::ValueType::Int64, static_cast<int64_t>(std::stoll(tok.value)));
        return lit;
    }

    if (check(TokenType::Float))
    {
        Token tok = advance_token();
        auto lit = std::make_shared<LiteralExpr>();
        lit->value = type::Value(type::ValueType::Float64, std::stod(tok.value));
        return lit;
    }

    if (check(TokenType::String))
    {
        Token tok = advance_token();
        auto lit = std::make_shared<LiteralExpr>();
        lit->value = type::Value(type::ValueType::String, tok.value);
        return lit;
    }

    if (match(TokenType::True))
    {
        auto lit = std::make_shared<LiteralExpr>();
        lit->value = type::Value(type::ValueType::Bool, true);
        return lit;
    }

    if (match(TokenType::False))
    {
        auto lit = std::make_shared<LiteralExpr>();
        lit->value = type::Value(type::ValueType::Bool, false);
        return lit;
    }

    if (match(TokenType::Null))
    {
        auto lit = std::make_shared<LiteralExpr>();
        lit->value = type::Value();
        return lit;
    }

    if (match(TokenType::Star))
    {
        return std::make_shared<StarExpr>();
    }

    if (check(TokenType::Identifier) || check(TokenType::Count) || check(TokenType::Sum) || check(TokenType::Avg) || check(TokenType::Min)
        || check(TokenType::Max))
    {
        Token tok = advance_token();
        if (check(TokenType::LParen))
        {
            return maybe_window(parse_function_call(tok.value));
        }
        if (match(TokenType::Dot))
        {
            if (match(TokenType::Star))
            {
                auto star = std::make_shared<StarExpr>();
                star->table_name = tok.value;
                return star;
            }
            Token col = expect(TokenType::Identifier);
            auto col_ref = std::make_shared<ColumnRefExpr>();
            col_ref->table_name = tok.value;
            col_ref->column_name = col.value;
            return col_ref;
        }
        auto col_ref = std::make_shared<ColumnRefExpr>();
        col_ref->column_name = tok.value;
        return col_ref;
    }

    if (check(TokenType::Cast))
    {
        advance_token();
        expect(TokenType::LParen);
        auto cast_expr = std::make_shared<CastExpr>();
        cast_expr->operand = parse_expr();
        expect(TokenType::As);
        cast_expr->target = parse_cast_target();
        expect(TokenType::RParen);
        return cast_expr;
    }

    if (check(TokenType::Case))
    {
        return parse_case_expr();
    }

    if (match(TokenType::Exists))
    {
        expect(TokenType::LParen);
        expect(TokenType::Select);
        auto exists = std::make_shared<ExistsExpr>();
        exists->query = parse_select();
        expect(TokenType::RParen);
        return exists;
    }

    if (match(TokenType::LParen))
    {
        if (match(TokenType::Select))
        {
            auto subquery = std::make_shared<SubqueryExpr>();
            subquery->query = parse_select();
            expect(TokenType::RParen);
            return subquery;
        }
        auto expr = parse_expr();
        expect(TokenType::RParen);
        return expr;
    }

    throw ParseError(std::format("Unexpected token '{}' in expression", current().value));
}

ExprPtr Parser::parse_case_expr()
{
    expect(TokenType::Case);
    auto case_expr = std::make_shared<CaseExpr>();
    if (!check(TokenType::When))
    {
        case_expr->operand = parse_expr();
    }
    while (match(TokenType::When))
    {
        auto cond = parse_expr();
        expect(TokenType::Then);
        auto result = parse_expr();
        case_expr->whens.emplace_back(cond, result);
    }
    if (case_expr->whens.empty())
    {
        throw ParseError("CASE requires at least one WHEN clause");
    }
    if (match(TokenType::Else))
    {
        case_expr->else_expr = parse_expr();
    }
    expect(TokenType::End);
    return case_expr;
}

ExprPtr Parser::parse_comparison_rhs(BinaryOpType op, const ExprPtr & left)
{
    if (check(TokenType::Any) || check(TokenType::All) || check(TokenType::Some))
    {
        bool all = check(TokenType::All);
        advance_token();
        expect(TokenType::LParen);
        expect(TokenType::Select);
        auto quantified = std::make_shared<QuantifiedExpr>();
        quantified->operand = left;
        quantified->op = op;
        quantified->all = all;
        quantified->query = parse_select();
        expect(TokenType::RParen);
        return quantified;
    }
    auto binary = std::make_shared<BinaryOpExpr>();
    binary->op = op;
    binary->left = left;
    binary->right = parse_addition();
    return binary;
}

popo::type::ValueType Parser::parse_cast_target()
{
    if (match(TokenType::Int) || match(TokenType::Integer_))
        return popo::type::ValueType::Int32;
    if (match(TokenType::Bigint))
        return popo::type::ValueType::Int64;
    if (match(TokenType::Smallint))
        return popo::type::ValueType::Int16;
    if (match(TokenType::Tinyint))
        return popo::type::ValueType::Int8;
    if (match(TokenType::Float_))
        return popo::type::ValueType::Float32;
    if (match(TokenType::Double))
        return popo::type::ValueType::Float64;
    if (match(TokenType::Boolean) || match(TokenType::Bool_))
        return popo::type::ValueType::Bool;
    if (match(TokenType::Varchar) || match(TokenType::Char) || match(TokenType::Text))
    {
        if (match(TokenType::LParen))
        {
            expect(TokenType::Integer);
            expect(TokenType::RParen);
        }
        return popo::type::ValueType::String;
    }
    throw ParseError(std::format("Unsupported CAST target '{}'", current().value));
}

ExprPtr Parser::maybe_window(ExprPtr func)
{
    if (!match(TokenType::Over))
    {
        return func;
    }
    expect(TokenType::LParen);
    auto window = std::make_shared<WindowExpr>();
    window->function = std::move(func);
    if (match(TokenType::Partition))
    {
        expect(TokenType::By);
        do
        {
            window->partition_by.push_back(parse_expr());
        } while (match(TokenType::Comma));
    }
    if (match(TokenType::Order))
    {
        expect(TokenType::By);
        window->order_by = parse_order_by();
    }
    expect(TokenType::RParen);
    return window;
}

ExprPtr Parser::parse_function_call(const std::string & name)
{
    expect(TokenType::LParen);

    std::string upper_name;
    for (char c : name)
    {
        upper_name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    if (upper_name == "XMLELEMENT")
    {
        auto call = std::make_shared<ScalarCallExpr>();
        call->name = upper_name;
        if (check(TokenType::Identifier))
        {
            std::string word = current().value;
            for (char & ch : word)
            {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (word == "name")
            {
                advance_token();
            }
        }
        auto tag = std::make_shared<LiteralExpr>();
        tag->value = popo::type::Value(popo::type::ValueType::String, expect(TokenType::Identifier).value);
        call->args.push_back(tag);
        if (match(TokenType::Comma))
        {
            call->args.push_back(parse_expr());
        }
        expect(TokenType::RParen);
        return call;
    }

    if (upper_name == "XMLFOREST")
    {
        auto call = std::make_shared<ScalarCallExpr>();
        call->name = upper_name;
        if (!check(TokenType::RParen))
        {
            do
            {
                auto expr = parse_expr();
                std::string alias;
                if (match(TokenType::As))
                {
                    alias = expect(TokenType::Identifier).value;
                }
                else if (expr->type == ExprType::ColumnRef)
                {
                    alias = static_cast<const ColumnRefExpr &>(*expr).column_name;
                }
                auto name_lit = std::make_shared<LiteralExpr>();
                name_lit->value = popo::type::Value(popo::type::ValueType::String, alias);
                call->args.push_back(name_lit);
                call->args.push_back(expr);
            } while (match(TokenType::Comma));
        }
        expect(TokenType::RParen);
        return call;
    }

    bool aggregate = upper_name == "COUNT" || upper_name == "SUM" || upper_name == "AVG" || upper_name == "MIN" || upper_name == "MAX"
        || upper_name == "XMLAGG";
    if (!aggregate)
    {
        auto call = std::make_shared<ScalarCallExpr>();
        call->name = upper_name;
        if (!check(TokenType::RParen))
        {
            do
            {
                call->args.push_back(parse_expr());
            } while (match(TokenType::Comma));
        }
        expect(TokenType::RParen);
        return call;
    }

    auto func = std::make_shared<FunctionCallExpr>();

    if (upper_name == "COUNT")
        func->func = AggFuncType::Count;
    else if (upper_name == "SUM")
        func->func = AggFuncType::Sum;
    else if (upper_name == "AVG")
        func->func = AggFuncType::Avg;
    else if (upper_name == "MIN")
        func->func = AggFuncType::Min;
    else if (upper_name == "MAX")
        func->func = AggFuncType::Max;
    else
        func->func = AggFuncType::XmlAgg;

    if (match(TokenType::Distinct))
    {
        func->is_distinct = true;
    }

    if (func->func == AggFuncType::Count && check(TokenType::Star))
    {
        advance_token();
        func->args.push_back(std::make_shared<StarExpr>());
    }
    else
    {
        if (!check(TokenType::RParen))
        {
            do
            {
                func->args.push_back(parse_expr());
            } while (match(TokenType::Comma));
        }
    }

    expect(TokenType::RParen);
    return func;
}

// CREATE TABLE helpers

ColumnDef Parser::parse_column_def()
{
    Token name = expect(TokenType::Identifier);
    ColumnDef def;
    def.name = name.value;
    def.data_type = parse_data_type(def.type_len);

    while (check(TokenType::Primary) || check(TokenType::Not) || check(TokenType::Unique) || check(TokenType::Default))
    {
        if (match(TokenType::Primary))
        {
            expect(TokenType::Key);
            def.is_primary_key = true;
            def.is_nullable = false;
        }
        else if (match(TokenType::Not))
        {
            expect(TokenType::Null);
            def.is_nullable = false;
        }
        else if (match(TokenType::Unique))
        {
            def.is_unique = true;
        }
        else if (match(TokenType::Default))
        {
            auto expr = parse_expr();
            if (expr && expr->type == ExprType::Literal)
            {
                auto * lit = static_cast<LiteralExpr *>(expr.get());
                def.default_value = lit->value;
            }
            else
            {
                throw ParseError("DEFAULT value must be a literal");
            }
        }
    }

    return def;
}

popo::type::ValueType Parser::parse_data_type(int32_t & type_len)
{
    if (match(TokenType::Int) || match(TokenType::Integer_))
    {
        return type::ValueType::Int32;
    }
    if (match(TokenType::Bigint))
    {
        return type::ValueType::Int64;
    }
    if (match(TokenType::Smallint))
    {
        return type::ValueType::Int16;
    }
    if (match(TokenType::Tinyint))
    {
        return type::ValueType::Int8;
    }
    if (match(TokenType::Float_))
    {
        return type::ValueType::Float32;
    }
    if (match(TokenType::Double))
    {
        return type::ValueType::Float64;
    }
    if (match(TokenType::Varchar) || match(TokenType::Char))
    {
        if (match(TokenType::LParen))
        {
            Token len = expect(TokenType::Integer);
            type_len = static_cast<int32_t>(std::stoll(len.value));
            expect(TokenType::RParen);
        }
        return type::ValueType::String;
    }
    if (match(TokenType::Text))
    {
        return type::ValueType::String;
    }
    if (match(TokenType::Boolean) || match(TokenType::Bool_))
    {
        return type::ValueType::Bool;
    }
    throw ParseError(std::format("Unknown data type: '{}'", current().value));
}

StmtPtr Parser::parse_show()
{
    if (match(TokenType::Tables))
    {
        return std::make_shared<ShowTablesStatement>();
    }
    if (match(TokenType::Databases))
    {
        return std::make_shared<ShowDatabasesStatement>();
    }
    if (match(TokenType::Variables))
    {
        return std::make_shared<ShowVariablesStatement>();
    }
    if (match(TokenType::Status))
    {
        return std::make_shared<ShowStatusStatement>();
    }
    // SHOW without recognized sub-command: treat as show variables (safe fallback)
    return std::make_shared<ShowVariablesStatement>();
}

StmtPtr Parser::parse_describe()
{
    Token table = expect(TokenType::Identifier);
    auto stmt = std::make_shared<DescribeStatement>();
    stmt->table_name = table.value;
    return stmt;
}

StmtPtr Parser::parse_set()
{
    auto stmt = std::make_shared<SetVarStatement>();
    // Consume the rest of the SET statement (all tokens until semicolon or eof)
    // We don't currently support actual SET variables, but we parse them gracefully
    while (!is_at_end() && current().type != TokenType::Semicolon)
    {
        if (!stmt->var_name.empty())
            stmt->var_name += ' ';
        stmt->var_name += advance_token().value;
    }
    return stmt;
}

StmtPtr Parser::parse_select_database()
{
    // SELECT DATABASE()
    advance_token(); // consume DATABASE identifier
    expect(TokenType::LParen);
    expect(TokenType::RParen);
    return std::make_shared<SelectDatabaseStatement>();
}

StmtPtr Parser::parse_select_system_var()
{
    // SELECT @@xxx
    auto stmt = std::make_shared<SelectSystemVarStatement>();
    stmt->var_name = advance_token().value; // consume SystemVar token
    return stmt;
}

} // namespace popo::parser
