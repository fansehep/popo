#include "parser/parser.h"

#include <cctype>
#include <format>
#include <memory>
#include <stdexcept>

namespace neon::parser
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

neon::base::Result<std::vector<StmtPtr>> Parser::parse(const std::string & sql)
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
        return parse_create_table();
    if (match(TokenType::Update))
        return parse_update();
    if (match(TokenType::Delete))
        return parse_delete();
    if (match(TokenType::Drop))
        return parse_drop();
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
    stmt->select_list = parse_select_list();

    expect(TokenType::From);
    stmt->from = parse_table_ref();

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

    if (match(TokenType::Order))
    {
        expect(TokenType::By);
        stmt->order_by = parse_order_by();
    }

    if (match(TokenType::Limit))
    {
        stmt->limit = parse_limit();
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

StmtPtr Parser::parse_drop()
{
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
    std::vector<ExprPtr> list;
    list.push_back(parse_expr());
    while (match(TokenType::Comma))
    {
        list.push_back(parse_expr());
    }
    return list;
}

TableRefPtr Parser::parse_table_ref()
{
    auto left = [&]() -> TableRefPtr
    {
        Token name = expect(TokenType::Identifier);
        auto ref = std::make_shared<SimpleTableRef>();
        ref->table_name = name.value;
        if (match(TokenType::As))
        {
            Token alias = expect(TokenType::Identifier);
            ref->alias = alias.value;
        }
        else if (check(TokenType::Identifier))
        {
            ref->alias = advance_token().value;
        }
        return ref;
    }();

    while (true)
    {
        if (check(TokenType::Comma))
        {
            advance_token();
            auto right = [&]() -> TableRefPtr
            {
                Token name = expect(TokenType::Identifier);
                auto ref = std::make_shared<SimpleTableRef>();
                ref->table_name = name.value;
                if (match(TokenType::As))
                {
                    Token alias = expect(TokenType::Identifier);
                    ref->alias = alias.value;
                }
                else if (check(TokenType::Identifier))
                {
                    ref->alias = advance_token().value;
                }
                return ref;
            }();
            auto join = std::make_shared<JoinTableRef>();
            join->join_type = JoinType::Cross;
            join->left = left;
            join->right = right;
            left = join;
        }
        else if (
            check(TokenType::Join) || check(TokenType::Inner) || check(TokenType::Left) || check(TokenType::Right)
            || check(TokenType::Cross))
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
    }
    else if (check(TokenType::Right))
    {
        advance_token();
        join_type = JoinType::Right;
    }
    else if (check(TokenType::Cross))
    {
        advance_token();
        join_type = JoinType::Cross;
    }

    expect(TokenType::Join);

    auto right = [&]() -> TableRefPtr
    {
        Token name = expect(TokenType::Identifier);
        auto ref = std::make_shared<SimpleTableRef>();
        ref->table_name = name.value;
        if (match(TokenType::As))
        {
            Token alias = expect(TokenType::Identifier);
            ref->alias = alias.value;
        }
        else if (check(TokenType::Identifier))
        {
            ref->alias = advance_token().value;
        }
        return ref;
    }();

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
        if (match(TokenType::Eq))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Eq;
            binary->left = left;
            binary->right = parse_addition();
            left = binary;
        }
        else if (match(TokenType::Ne))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Ne;
            binary->left = left;
            binary->right = parse_addition();
            left = binary;
        }
        else if (match(TokenType::Lt))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Lt;
            binary->left = left;
            binary->right = parse_addition();
            left = binary;
        }
        else if (match(TokenType::Le))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Le;
            binary->left = left;
            binary->right = parse_addition();
            left = binary;
        }
        else if (match(TokenType::Gt))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Gt;
            binary->left = left;
            binary->right = parse_addition();
            left = binary;
        }
        else if (match(TokenType::Ge))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Ge;
            binary->left = left;
            binary->right = parse_addition();
            left = binary;
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
            return parse_function_call(tok.value);
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

    if (match(TokenType::LParen))
    {
        auto expr = parse_expr();
        expect(TokenType::RParen);
        return expr;
    }

    throw ParseError(std::format("Unexpected token '{}' in expression", current().value));
}

ExprPtr Parser::parse_function_call(const std::string & name)
{
    expect(TokenType::LParen);

    auto func = std::make_shared<FunctionCallExpr>();

    std::string upper_name;
    for (char c : name)
    {
        upper_name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

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
        func->func = AggFuncType::Count;

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

neon::type::ValueType Parser::parse_data_type(int32_t & type_len)
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

} // namespace neon::parser
