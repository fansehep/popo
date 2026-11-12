#include "parser/parser.h"

#include <cctype>
#include <format>
#include <memory>

namespace popo::parser
{

namespace
{
std::unexpected<base::Error> parse_error(std::string message)
{
    return base::make_error(base::Errno::InvalidArg, std::move(message));
}

#define PARSER_TRY(expression)                                                                                       \
    ({                                                                                                               \
        auto _parser_result = (expression);                                                                          \
        if (!_parser_result)                                                                                         \
            return std::unexpected(std::move(_parser_result).error());                                               \
        std::move(*_parser_result);                                                                                  \
    })

bool keyword_is(const Token & tok, const char * word)
{
    if (tok.type == TokenType::String || tok.type == TokenType::Integer || tok.type == TokenType::Float || tok.type == TokenType::Eof)
    {
        return false;
    }
    std::string_view text = tok.value;
    std::string_view want = word;
    if (text.size() != want.size())
    {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(text[i])) != want[i])
        {
            return false;
        }
    }
    return true;
}

std::string lowercased(const std::string & text)
{
    std::string out = text;
    for (char & ch : out)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

base::Result<std::string> canonical_date_string(const std::string & raw)
{
    int y = 0;
    int m = 0;
    int d = 0;
    size_t p = 0;
    while (p < raw.size() && std::isspace(static_cast<unsigned char>(raw[p])))
        ++p;
    auto read_int = [&](int & out) -> bool
    {
        if (p >= raw.size() || !std::isdigit(static_cast<unsigned char>(raw[p])))
            return false;
        int v = 0;
        while (p < raw.size() && std::isdigit(static_cast<unsigned char>(raw[p])))
        {
            v = v * 10 + (raw[p] - '0');
            ++p;
        }
        out = v;
        return true;
    };
    if (!read_int(y) || p >= raw.size() || raw[p] != '-')
        return parse_error(std::format("Invalid DATE literal '{}'", raw));
    ++p;
    if (!read_int(m) || p >= raw.size() || raw[p] != '-')
        return parse_error(std::format("Invalid DATE literal '{}'", raw));
    ++p;
    if (!read_int(d))
        return parse_error(std::format("Invalid DATE literal '{}'", raw));
    while (p < raw.size() && std::isspace(static_cast<unsigned char>(raw[p])))
        ++p;
    if (p != raw.size() || m < 1 || m > 12 || d < 1 || d > 31)
        return parse_error(std::format("Invalid DATE literal '{}'", raw));
    return std::format("{:04d}-{:02d}-{:02d}", y, m, d);
}
} // namespace

bool Parser::check_keyword(const char * word) const
{
    return keyword_is(current(), word);
}

bool Parser::match_keyword(const char * word)
{
    if (!check_keyword(word))
    {
        return false;
    }
    advance_token();
    return true;
}

popo::base::Result<void> Parser::expect_keyword(const char * word)
{
    if (!match_keyword(word))
    {
        return parse_error(std::format("Expected {}, got '{}'", word, current().value));
    }
    return {};
}

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

popo::base::Result<Token> Parser::expect(TokenType type)
{
    if (!check(type))
    {
        return parse_error(std::format("Expected token type {}, got '{}'", static_cast<int>(type), current().value));
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
        auto stmt = PARSER_TRY(parse_statement());
        if (stmt)
        {
            statements.push_back(std::move(stmt));
        }
        match(TokenType::Semicolon);
    }
    return statements;
}

// Statement parsing

popo::base::Result<StmtPtr> Parser::parse_statement()
{
    if (check_keyword("begin"))
        return PARSER_TRY(parse_begin());
    if (check_keyword("start"))
        return PARSER_TRY(parse_start_transaction());
    if (check_keyword("commit"))
        return PARSER_TRY(parse_commit());
    if (check_keyword("rollback"))
        return PARSER_TRY(parse_rollback());
    if (match(TokenType::With))
    {
        bool recursive = match(TokenType::Recursive);
        auto ctes = PARSER_TRY(parse_with_clause());
        for (auto & cte : ctes)
        {
            cte.recursive = recursive;
        }
        PARSER_TRY(expect(TokenType::Select));
        auto stmt = std::dynamic_pointer_cast<SelectStatement>(PARSER_TRY(parse_select()));
        stmt->with_clause = std::move(ctes);
        return stmt;
    }
    if (match(TokenType::Select))
    {
        // Check for SELECT DATABASE()
        if (current().type == TokenType::Identifier && (current().value == "database" || current().value == "DATABASE")
            && peek_token().type == TokenType::LParen)
        {
            return PARSER_TRY(parse_select_database());
        }
        // Check for SELECT @@xxx
        if (current().type == TokenType::SystemVar)
        {
            return PARSER_TRY(parse_select_system_var());
        }
        return PARSER_TRY(parse_select());
    }
    if (match(TokenType::Insert))
        return PARSER_TRY(parse_insert());
    if (match(TokenType::Create))
    {
        bool or_replace = false;
        if (match(TokenType::Or))
        {
            POPO_TRY(expect_keyword("replace"));
            or_replace = true;
        }
        if (check(TokenType::Unique))
        {
            advance_token();
            return PARSER_TRY(parse_create_index(true));
        }
        if (check(TokenType::Index))
        {
            return PARSER_TRY(parse_create_index(false));
        }
        if (match(TokenType::View))
        {
            return PARSER_TRY(parse_create_view(or_replace));
        }
        if (match(TokenType::User))
        {
            return PARSER_TRY(parse_create_user());
        }
        return PARSER_TRY(parse_create_table());
    }
    if (match(TokenType::Grant))
        return PARSER_TRY(parse_grant());
    if (match(TokenType::Revoke))
        return PARSER_TRY(parse_revoke());
    if (match(TokenType::Update))
        return PARSER_TRY(parse_update());
    if (match(TokenType::Delete))
        return PARSER_TRY(parse_delete());
    if (match(TokenType::Drop))
        return PARSER_TRY(parse_drop());
    if (match(TokenType::Alter))
    {
        if (match(TokenType::User))
            return PARSER_TRY(parse_alter_user());
        return PARSER_TRY(parse_alter_table());
    }
    if (match(TokenType::Show))
        return PARSER_TRY(parse_show());
    if (match(TokenType::Desc) || match(TokenType::Describe))
        return PARSER_TRY(parse_describe());
    if (match(TokenType::Set))
    {
        if (check(TokenType::Password))
            return PARSER_TRY(parse_set_password());
        return PARSER_TRY(parse_set());
    }
    return parse_error(std::format("Unexpected token '{}', expected statement", current().value));
}

popo::base::Result<StmtPtr> Parser::parse_select()
{
    auto stmt = std::make_shared<SelectStatement>();
    POPO_TRY(parse_select_core(stmt));

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
            PARSER_TRY(expect(TokenType::Union));
            term.kind = SetOpKind::Union;
        }
        term.is_all = match(TokenType::All);
        PARSER_TRY(expect(TokenType::Select));
        term.query = std::make_shared<SelectStatement>();
        POPO_TRY(parse_select_core(term.query));
        stmt->set_ops.push_back(std::move(term));
    }

    if (match(TokenType::Order))
    {
        PARSER_TRY(expect(TokenType::By));
        stmt->order_by = PARSER_TRY(parse_order_by());
    }

    if (match(TokenType::Limit))
    {
        stmt->limit = PARSER_TRY(parse_limit());
    }

    if (match(TokenType::Offset))
    {
        stmt->offset = PARSER_TRY(parse_limit());
    }

    return stmt;
}

popo::base::Result<void> Parser::parse_select_core(const std::shared_ptr<SelectStatement> & stmt)
{
    if (match(TokenType::Distinct))
    {
        stmt->distinct = true;
    }
    else
    {
        match(TokenType::All);
    }
    stmt->select_list = PARSER_TRY(parse_select_list());

    if (match(TokenType::From))
    {
        stmt->from = PARSER_TRY(parse_table_ref());
    }

    if (match(TokenType::Where))
    {
        stmt->where = PARSER_TRY(parse_expr());
    }

    if (match(TokenType::Group))
    {
        PARSER_TRY(expect(TokenType::By));
        stmt->group_by = PARSER_TRY(parse_group_by());
    }

    if (match(TokenType::Having))
    {
        stmt->having = PARSER_TRY(parse_expr());
    }
    return {};
}

popo::base::Result<std::vector<CteDef>> Parser::parse_with_clause()
{
    std::vector<CteDef> ctes;
    do
    {
        CteDef cte;
        cte.name = PARSER_TRY(expect(TokenType::Identifier)).value;
        if (match(TokenType::LParen))
        {
            do
            {
                cte.column_names.push_back(PARSER_TRY(expect(TokenType::Identifier)).value);
            } while (match(TokenType::Comma));
            PARSER_TRY(expect(TokenType::RParen));
        }
        PARSER_TRY(expect(TokenType::As));
        PARSER_TRY(expect(TokenType::LParen));
        PARSER_TRY(expect(TokenType::Select));
        cte.query = std::dynamic_pointer_cast<SelectStatement>(PARSER_TRY(parse_select()));
        PARSER_TRY(expect(TokenType::RParen));
        ctes.push_back(std::move(cte));
    } while (match(TokenType::Comma));
    return ctes;
}

popo::base::Result<void> Parser::skip_parenthesized()
{
    PARSER_TRY(expect(TokenType::LParen));
    int depth = 1;
    while (!is_at_end() && depth > 0)
    {
        if (match(TokenType::LParen))
            ++depth;
        else if (match(TokenType::RParen))
            --depth;
        else
            advance_token();
    }
    if (depth != 0)
    {
        return parse_error("Unclosed '('");
    }
    return {};
}

popo::base::Result<void> Parser::parse_foreign_key_tail()
{
    POPO_TRY(expect_keyword("references"));
    PARSER_TRY(expect(TokenType::Identifier));
    if (check(TokenType::LParen))
    {
        POPO_TRY(skip_parenthesized());
    }
    while (match(TokenType::On))
    {
        if (match(TokenType::Delete) || match(TokenType::Update))
        {
            if (match(TokenType::Set))
            {
                if (!match(TokenType::Null) && !match(TokenType::Default))
                {
                    advance_token();
                }
            }
            else if (match_keyword("no"))
            {
                POPO_TRY(expect_keyword("action"));
            }
            else
            {
                advance_token(); // CASCADE / RESTRICT
            }
        }
        else
        {
            break;
        }
    }
    return {};
}

popo::base::Result<StmtPtr> Parser::parse_alter_table()
{
    PARSER_TRY(expect(TokenType::Table));
    auto stmt = std::make_shared<AlterTableStatement>();
    stmt->table_name = PARSER_TRY(expect(TokenType::Identifier)).value;

    if (match(TokenType::Add))
    {
        if (match_keyword("constraint"))
        {
            if (!check(TokenType::Primary) && !check(TokenType::Unique) && !check_keyword("foreign") && !check_keyword("check"))
            {
                PARSER_TRY(expect(TokenType::Identifier));
            }
        }
        if (match(TokenType::Primary))
        {
            PARSER_TRY(expect(TokenType::Key));
            PARSER_TRY(expect(TokenType::LParen));
            do
            {
                stmt->key_columns.push_back(PARSER_TRY(expect(TokenType::Identifier)).value);
            } while (match(TokenType::Comma));
            PARSER_TRY(expect(TokenType::RParen));
            stmt->action = AlterAction::AddPrimaryKey;
        }
        else if (match_keyword("foreign"))
        {
            PARSER_TRY(expect(TokenType::Key));
            if (check(TokenType::Identifier) && peek_token().type == TokenType::LParen)
            {
                advance_token();
            }
            POPO_TRY(skip_parenthesized());
            POPO_TRY(parse_foreign_key_tail());
            stmt->action = AlterAction::NoOp;
        }
        else if (match_keyword("check"))
        {
            POPO_TRY(skip_parenthesized());
            stmt->action = AlterAction::NoOp;
        }
        else if (match(TokenType::Unique))
        {
            match(TokenType::Index);
            match(TokenType::Key);
            if (check(TokenType::Identifier) && peek_token().type == TokenType::LParen)
            {
                advance_token();
            }
            POPO_TRY(skip_parenthesized());
            stmt->action = AlterAction::NoOp;
        }
        else
        {
            match(TokenType::Column);
            stmt->action = AlterAction::AddColumn;
            stmt->column = PARSER_TRY(parse_column_def());
        }
    }
    else if (match(TokenType::Drop))
    {
        if (match_keyword("constraint") || match_keyword("foreign"))
        {
            match(TokenType::Key);
            PARSER_TRY(expect(TokenType::Identifier));
            stmt->action = AlterAction::NoOp;
        }
        else
        {
            match(TokenType::Column);
            stmt->action = AlterAction::DropColumn;
            stmt->target_name = PARSER_TRY(expect(TokenType::Identifier)).value;
        }
    }
    else if (match(TokenType::Rename))
    {
        PARSER_TRY(expect(TokenType::To));
        stmt->action = AlterAction::RenameTable;
        stmt->target_name = PARSER_TRY(expect(TokenType::Identifier)).value;
    }
    else
    {
        return parse_error(std::format("Unsupported ALTER TABLE action near '{}'", current().value));
    }
    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_insert()
{
    PARSER_TRY(expect(TokenType::Into));
    Token table = PARSER_TRY(expect(TokenType::Identifier));

    auto stmt = std::make_shared<InsertStatement>();
    stmt->table_name = table.value;

    if (match(TokenType::LParen))
    {
        do
        {
            Token col = PARSER_TRY(expect(TokenType::Identifier));
            stmt->columns.push_back(col.value);
        } while (match(TokenType::Comma));
        PARSER_TRY(expect(TokenType::RParen));
    }

    PARSER_TRY(expect(TokenType::Values));

    do
    {
        PARSER_TRY(expect(TokenType::LParen));
        std::vector<ExprPtr> row;
        do
        {
            row.push_back(PARSER_TRY(parse_expr()));
        } while (match(TokenType::Comma));
        PARSER_TRY(expect(TokenType::RParen));
        stmt->values.push_back(std::move(row));
    } while (match(TokenType::Comma));

    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_create_table()
{
    PARSER_TRY(expect(TokenType::Table));
    if (match(TokenType::If))
    {
        PARSER_TRY(expect(TokenType::Not));
        PARSER_TRY(expect(TokenType::Exists));
    }
    Token table = PARSER_TRY(expect(TokenType::Identifier));

    auto stmt = std::make_shared<CreateTableStatement>();
    stmt->table_name = table.value;

    std::vector<std::string> table_pk_cols;
    std::vector<std::string> table_unique_cols;

    PARSER_TRY(expect(TokenType::LParen));
    while (!check(TokenType::RParen))
    {
        if (match_keyword("constraint"))
        {
            if (!check(TokenType::Primary) && !check(TokenType::Unique) && !check_keyword("foreign") && !check_keyword("check"))
            {
                PARSER_TRY(expect(TokenType::Identifier));
            }
        }
        if (match(TokenType::Primary))
        {
            PARSER_TRY(expect(TokenType::Key));
            PARSER_TRY(expect(TokenType::LParen));
            do
            {
                table_pk_cols.push_back(PARSER_TRY(expect(TokenType::Identifier)).value);
            } while (match(TokenType::Comma));
            PARSER_TRY(expect(TokenType::RParen));
        }
        else if (match(TokenType::Unique))
        {
            match(TokenType::Index);
            match(TokenType::Key);
            if (check(TokenType::Identifier) && peek_token().type == TokenType::LParen)
            {
                advance_token();
            }
            PARSER_TRY(expect(TokenType::LParen));
            std::vector<std::string> ucols;
            do
            {
                ucols.push_back(PARSER_TRY(expect(TokenType::Identifier)).value);
            } while (match(TokenType::Comma));
            PARSER_TRY(expect(TokenType::RParen));
            if (ucols.size() == 1)
            {
                table_unique_cols.push_back(ucols[0]);
            }
        }
        else if (match_keyword("foreign"))
        {
            PARSER_TRY(expect(TokenType::Key));
            if (check(TokenType::Identifier) && peek_token().type == TokenType::LParen)
            {
                advance_token();
            }
            POPO_TRY(skip_parenthesized());
            POPO_TRY(parse_foreign_key_tail());
        }
        else if (match_keyword("check"))
        {
            POPO_TRY(skip_parenthesized());
        }
        else
        {
            stmt->columns.push_back(PARSER_TRY(parse_column_def()));
        }
        if (!match(TokenType::Comma))
        {
            break;
        }
    }
    PARSER_TRY(expect(TokenType::RParen));

    for (const auto & pk_col : table_pk_cols)
    {
        bool matched = false;
        for (auto & col : stmt->columns)
        {
            if (lowercased(col.name) == lowercased(pk_col))
            {
                col.is_primary_key = true;
                col.is_nullable = false;
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            return parse_error(std::format("PRIMARY KEY column '{}' not found in table '{}'", pk_col, stmt->table_name));
        }
    }
    for (const auto & u_col : table_unique_cols)
    {
        for (auto & col : stmt->columns)
        {
            if (lowercased(col.name) == lowercased(u_col))
            {
                col.is_unique = true;
            }
        }
    }

    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_create_index(bool unique)
{
    PARSER_TRY(expect(TokenType::Index));
    Token name = PARSER_TRY(expect(TokenType::Identifier));
    PARSER_TRY(expect(TokenType::On));
    Token table = PARSER_TRY(expect(TokenType::Identifier));
    PARSER_TRY(expect(TokenType::LParen));
    auto stmt = std::make_shared<CreateIndexStatement>();
    stmt->unique = unique;
    stmt->index_name = name.value;
    stmt->table_name = table.value;
    do
    {
        stmt->columns.push_back(PARSER_TRY(expect(TokenType::Identifier)).value);
    } while (match(TokenType::Comma));
    PARSER_TRY(expect(TokenType::RParen));
    if (stmt->columns.size() != 1)
    {
        return parse_error("only a single-column secondary index is supported");
    }
    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_update()
{
    Token table = PARSER_TRY(expect(TokenType::Identifier));

    auto stmt = std::make_shared<UpdateStatement>();
    stmt->table_name = table.value;

    PARSER_TRY(expect(TokenType::Set));

    do
    {
        Token col = PARSER_TRY(expect(TokenType::Identifier));
        PARSER_TRY(expect(TokenType::Eq));
        UpdateAssignment assign;
        assign.column_name = col.value;
        assign.value = PARSER_TRY(parse_expr());
        stmt->assignments.push_back(std::move(assign));
    } while (match(TokenType::Comma));

    if (match(TokenType::Where))
    {
        stmt->where = PARSER_TRY(parse_expr());
    }

    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_delete()
{
    PARSER_TRY(expect(TokenType::From));
    Token table = PARSER_TRY(expect(TokenType::Identifier));

    auto stmt = std::make_shared<DeleteStatement>();
    stmt->table_name = table.value;

    if (match(TokenType::Where))
    {
        stmt->where = PARSER_TRY(parse_expr());
    }

    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_create_view(bool or_replace)
{
    auto stmt = std::make_shared<CreateViewStatement>();
    stmt->or_replace = or_replace;
    stmt->name = PARSER_TRY(expect(TokenType::Identifier)).value;
    if (match(TokenType::LParen))
    {
        do
        {
            stmt->column_names.push_back(PARSER_TRY(expect(TokenType::Identifier)).value);
        } while (match(TokenType::Comma));
        PARSER_TRY(expect(TokenType::RParen));
    }
    PARSER_TRY(expect(TokenType::As));
    if (match(TokenType::With))
    {
        bool recursive = match(TokenType::Recursive);
        auto ctes = PARSER_TRY(parse_with_clause());
        for (auto & cte : ctes)
            cte.recursive = recursive;
        PARSER_TRY(expect(TokenType::Select));
        auto sel = std::dynamic_pointer_cast<SelectStatement>(PARSER_TRY(parse_select()));
        sel->with_clause = std::move(ctes);
        stmt->query = std::move(sel);
    }
    else
    {
        PARSER_TRY(expect(TokenType::Select));
        stmt->query = PARSER_TRY(parse_select());
    }
    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_drop()
{
    if (match(TokenType::View))
    {
        auto stmt = std::make_shared<DropViewStatement>();
        if (match(TokenType::If))
        {
            PARSER_TRY(expect(TokenType::Exists));
            stmt->if_exists = true;
        }
        stmt->name = PARSER_TRY(expect(TokenType::Identifier)).value;
        return stmt;
    }
    if (match(TokenType::User))
    {
        auto stmt = std::make_shared<DropUserStatement>();
        if (match(TokenType::If))
        {
            PARSER_TRY(expect(TokenType::Exists));
            stmt->if_exists = true;
        }
        do
        {
            stmt->users.push_back(PARSER_TRY(parse_account_name()));
        } while (match(TokenType::Comma));
        return stmt;
    }
    PARSER_TRY(expect(TokenType::Table));

    auto stmt = std::make_shared<DropTableStatement>();

    if (match(TokenType::If))
    {
        PARSER_TRY(expect(TokenType::Exists));
        stmt->if_exists = true;
    }

    Token table = PARSER_TRY(expect(TokenType::Identifier));
    stmt->table_name = table.value;

    return stmt;
}

// SELECT helpers

popo::base::Result<std::vector<ExprPtr>> Parser::parse_select_list()
{
    auto parse_item = [&]() -> base::Result<ExprPtr>
    {
        auto expr = PARSER_TRY(parse_expr());
        std::string alias;
        if (match(TokenType::As))
        {
            alias = PARSER_TRY(expect(TokenType::Identifier)).value;
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
    list.push_back(PARSER_TRY(parse_item()));
    while (match(TokenType::Comma))
    {
        list.push_back(PARSER_TRY(parse_item()));
    }
    return list;
}

popo::base::Result<TableRefPtr> Parser::parse_table_primary()
{
    if (match(TokenType::LParen))
    {
        auto ref = std::make_shared<DerivedTableRef>();
        if (match(TokenType::With))
        {
            bool recursive = match(TokenType::Recursive);
            auto ctes = PARSER_TRY(parse_with_clause());
            for (auto & cte : ctes)
                cte.recursive = recursive;
            PARSER_TRY(expect(TokenType::Select));
            auto sel = std::dynamic_pointer_cast<SelectStatement>(PARSER_TRY(parse_select()));
            sel->with_clause = std::move(ctes);
            ref->query = std::move(sel);
        }
        else
        {
            PARSER_TRY(expect(TokenType::Select));
            ref->query = PARSER_TRY(parse_select());
        }
        PARSER_TRY(expect(TokenType::RParen));
        if (match(TokenType::As))
        {
            ref->alias = PARSER_TRY(expect(TokenType::Identifier)).value;
        }
        else if (check(TokenType::Identifier))
        {
            ref->alias = advance_token().value;
        }
        if (match(TokenType::LParen))
        {
            do
            {
                ref->column_aliases.push_back(PARSER_TRY(expect(TokenType::Identifier)).value);
            } while (match(TokenType::Comma));
            PARSER_TRY(expect(TokenType::RParen));
        }
        return ref;
    }
    Token name = PARSER_TRY(expect(TokenType::Identifier));
    auto ref = std::make_shared<SimpleTableRef>();
    ref->table_name = name.value;
    if (match(TokenType::As))
    {
        ref->alias = PARSER_TRY(expect(TokenType::Identifier)).value;
    }
    else if (check(TokenType::Identifier))
    {
        ref->alias = advance_token().value;
    }
    if (match(TokenType::LParen))
    {
        do
        {
            ref->column_aliases.push_back(PARSER_TRY(expect(TokenType::Identifier)).value);
        } while (match(TokenType::Comma));
        PARSER_TRY(expect(TokenType::RParen));
    }
    return ref;
}

popo::base::Result<TableRefPtr> Parser::parse_table_ref()
{
    auto left = PARSER_TRY(parse_table_primary());

    while (true)
    {
        if (check(TokenType::Comma))
        {
            advance_token();
            auto right = PARSER_TRY(parse_table_primary());
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
            left = PARSER_TRY(parse_join(left));
        }
        else
        {
            break;
        }
    }

    return left;
}

popo::base::Result<TableRefPtr> Parser::parse_join(TableRefPtr left)
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

    PARSER_TRY(expect(TokenType::Join));

    auto right = PARSER_TRY(parse_table_primary());

    ExprPtr condition = nullptr;
    if (join_type != JoinType::Cross)
    {
        if (match(TokenType::On))
        {
            condition = PARSER_TRY(parse_expr());
        }
    }

    auto join = std::make_shared<JoinTableRef>();
    join->join_type = join_type;
    join->left = left;
    join->right = right;
    join->condition = condition;
    return join;
}

popo::base::Result<ExprPtr> Parser::parse_where()
{
    return PARSER_TRY(parse_expr());
}

popo::base::Result<std::vector<ExprPtr>> Parser::parse_group_by()
{
    std::vector<ExprPtr> list;
    list.push_back(PARSER_TRY(parse_expr()));
    while (match(TokenType::Comma))
    {
        list.push_back(PARSER_TRY(parse_expr()));
    }
    return list;
}

popo::base::Result<ExprPtr> Parser::parse_having()
{
    return PARSER_TRY(parse_expr());
}

popo::base::Result<std::vector<OrderByItem>> Parser::parse_order_by()
{
    std::vector<OrderByItem> list;
    do
    {
        OrderByItem item;
        item.expr = PARSER_TRY(parse_expr());
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

popo::base::Result<int64_t> Parser::parse_limit()
{
    Token tok = PARSER_TRY(expect(TokenType::Integer));
    return PARSER_TRY(base::parse_i64(tok.value));
}

// Expression parsing (precedence climbing)

popo::base::Result<ExprPtr> Parser::parse_expr()
{
    return PARSER_TRY(parse_or_expr());
}

popo::base::Result<ExprPtr> Parser::parse_or_expr()
{
    auto left = PARSER_TRY(parse_and_expr());
    while (match(TokenType::Or))
    {
        auto binary = std::make_shared<BinaryOpExpr>();
        binary->op = BinaryOpType::Or;
        binary->left = left;
        binary->right = PARSER_TRY(parse_and_expr());
        left = binary;
    }
    return left;
}

popo::base::Result<ExprPtr> Parser::parse_and_expr()
{
    auto left = PARSER_TRY(parse_not_expr());
    while (match(TokenType::And))
    {
        auto binary = std::make_shared<BinaryOpExpr>();
        binary->op = BinaryOpType::And;
        binary->left = left;
        binary->right = PARSER_TRY(parse_not_expr());
        left = binary;
    }
    return left;
}

popo::base::Result<ExprPtr> Parser::parse_not_expr()
{
    if (match(TokenType::Not))
    {
        auto unary = std::make_shared<UnaryOpExpr>();
        unary->op = UnaryOpType::Not;
        unary->operand = PARSER_TRY(parse_not_expr());
        return unary;
    }
    return PARSER_TRY(parse_comparison());
}

popo::base::Result<ExprPtr> Parser::parse_comparison()
{
    auto left = PARSER_TRY(parse_addition());
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
                PARSER_TRY(expect(TokenType::LParen));
                if (check(TokenType::Select))
                {
                    advance_token();
                    auto in_sub = std::make_shared<InSubqueryExpr>();
                    in_sub->operand = left;
                    in_sub->query = PARSER_TRY(parse_select());
                    in_sub->negated = negated;
                    PARSER_TRY(expect(TokenType::RParen));
                    left = in_sub;
                }
                else
                {
                    auto in = std::make_shared<InExpr>();
                    in->operand = left;
                    in->negated = negated;
                    do
                    {
                        in->list.push_back(PARSER_TRY(parse_expr()));
                    } while (match(TokenType::Comma));
                    PARSER_TRY(expect(TokenType::RParen));
                    left = in;
                }
            }
            else if (match(TokenType::Between))
            {
                auto between = std::make_shared<BetweenExpr>();
                between->operand = left;
                between->negated = negated;
                between->low = PARSER_TRY(parse_addition());
                PARSER_TRY(expect(TokenType::And));
                between->high = PARSER_TRY(parse_addition());
                left = between;
            }
            else if (match(TokenType::Like))
            {
                auto like = std::make_shared<LikeExpr>();
                like->operand = left;
                like->negated = negated;
                like->pattern = PARSER_TRY(parse_addition());
                left = like;
            }
            continue;
        }
        if (match(TokenType::Is))
        {
            bool negated = match(TokenType::Not);
            if (match(TokenType::Distinct))
            {
                PARSER_TRY(expect(TokenType::From));
                auto distinct = std::make_shared<DistinctFromExpr>();
                distinct->left = left;
                distinct->negated = negated;
                distinct->right = PARSER_TRY(parse_addition());
                left = distinct;
                continue;
            }
            auto is_null = std::make_shared<IsNullExpr>();
            is_null->operand = left;
            is_null->negated = negated;
            PARSER_TRY(expect(TokenType::Null));
            left = is_null;
            continue;
        }
        if (match(TokenType::Eq))
        {
            left = PARSER_TRY(parse_comparison_rhs(BinaryOpType::Eq, left));
        }
        else if (match(TokenType::Ne))
        {
            left = PARSER_TRY(parse_comparison_rhs(BinaryOpType::Ne, left));
        }
        else if (match(TokenType::Lt))
        {
            left = PARSER_TRY(parse_comparison_rhs(BinaryOpType::Lt, left));
        }
        else if (match(TokenType::Le))
        {
            left = PARSER_TRY(parse_comparison_rhs(BinaryOpType::Le, left));
        }
        else if (match(TokenType::Gt))
        {
            left = PARSER_TRY(parse_comparison_rhs(BinaryOpType::Gt, left));
        }
        else if (match(TokenType::Ge))
        {
            left = PARSER_TRY(parse_comparison_rhs(BinaryOpType::Ge, left));
        }
        else
        {
            break;
        }
    }
    return left;
}

popo::base::Result<ExprPtr> Parser::parse_addition()
{
    auto left = PARSER_TRY(parse_multiplication());
    while (true)
    {
        if (match(TokenType::Plus))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Add;
            binary->left = left;
            binary->right = PARSER_TRY(parse_multiplication());
            left = binary;
        }
        else if (match(TokenType::Minus))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Sub;
            binary->left = left;
            binary->right = PARSER_TRY(parse_multiplication());
            left = binary;
        }
        else if (match(TokenType::Concat))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Concat;
            binary->left = left;
            binary->right = PARSER_TRY(parse_multiplication());
            left = binary;
        }
        else
        {
            break;
        }
    }
    return left;
}

popo::base::Result<ExprPtr> Parser::parse_multiplication()
{
    auto left = PARSER_TRY(parse_unary());
    while (true)
    {
        if (match(TokenType::Star))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Mul;
            binary->left = left;
            binary->right = PARSER_TRY(parse_unary());
            left = binary;
        }
        else if (match(TokenType::Slash))
        {
            auto binary = std::make_shared<BinaryOpExpr>();
            binary->op = BinaryOpType::Div;
            binary->left = left;
            binary->right = PARSER_TRY(parse_unary());
            left = binary;
        }
        else
        {
            break;
        }
    }
    return left;
}

popo::base::Result<ExprPtr> Parser::parse_unary()
{
    if (match(TokenType::Plus))
    {
        return PARSER_TRY(parse_unary());
    }
    if (match(TokenType::Minus))
    {
        auto unary = std::make_shared<UnaryOpExpr>();
        unary->op = UnaryOpType::Neg;
        unary->operand = PARSER_TRY(parse_unary());
        return unary;
    }
    if (match(TokenType::Not))
    {
        auto unary = std::make_shared<UnaryOpExpr>();
        unary->op = UnaryOpType::Not;
        unary->operand = PARSER_TRY(parse_unary());
        return unary;
    }
    return PARSER_TRY(parse_primary());
}

popo::base::Result<ExprPtr> Parser::parse_interval_expr()
{
    auto interval = std::make_shared<IntervalExpr>();
    interval->value = PARSER_TRY(parse_unary());
    if (match_keyword("year") || match_keyword("years"))
    {
        interval->unit = IntervalUnit::Year;
    }
    else if (match_keyword("month") || match_keyword("months"))
    {
        interval->unit = IntervalUnit::Month;
    }
    else if (match_keyword("day") || match_keyword("days"))
    {
        interval->unit = IntervalUnit::Day;
    }
    else if (
        interval->value && interval->value->type == ExprType::Literal
        && static_cast<const LiteralExpr &>(*interval->value).value.datatype() == type::ValueType::String)
    {
        // PostgreSQL / DuckDB single-string form: INTERVAL '90 days', INTERVAL '3 month', INTERVAL '1 year'
        const std::string raw = static_cast<const LiteralExpr &>(*interval->value).value.string_ref();
        size_t p = 0;
        while (p < raw.size() && std::isspace(static_cast<unsigned char>(raw[p])))
            ++p;
        size_t num_start = p;
        if (p < raw.size() && (raw[p] == '+' || raw[p] == '-'))
            ++p;
        size_t digits = 0;
        while (p < raw.size() && std::isdigit(static_cast<unsigned char>(raw[p])))
        {
            ++p;
            ++digits;
        }
        if (digits == 0)
            return parse_error(std::format("Invalid INTERVAL literal '{}'", raw));
        int64_t amount = PARSER_TRY(base::parse_i64(raw.substr(num_start, p - num_start)));
        while (p < raw.size() && std::isspace(static_cast<unsigned char>(raw[p])))
            ++p;
        std::string unit;
        while (p < raw.size() && std::isalpha(static_cast<unsigned char>(raw[p])))
        {
            unit.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw[p++]))));
        }
        if (unit == "year" || unit == "years")
            interval->unit = IntervalUnit::Year;
        else if (unit == "month" || unit == "months")
            interval->unit = IntervalUnit::Month;
        else if (unit == "day" || unit == "days")
            interval->unit = IntervalUnit::Day;
        else
            return parse_error(std::format("Unknown unit '{}' in INTERVAL literal '{}'", unit, raw));
        auto num_lit = std::make_shared<LiteralExpr>();
        num_lit->value = type::Value(type::ValueType::Int64, amount);
        interval->value = num_lit;
    }
    else
    {
        return parse_error(std::format("Expected YEAR, MONTH, or DAY in INTERVAL, got '{}'", current().value));
    }
    // Optional interval leading field precision, e.g. INTERVAL '90' DAY (3) in TPC-H Q1.
    if (match(TokenType::LParen))
    {
        PARSER_TRY(expect(TokenType::Integer));
        PARSER_TRY(expect(TokenType::RParen));
    }
    return interval;
}

popo::base::Result<ExprPtr> Parser::parse_primary()
{
    if (check_keyword("date") && peek_token().type == TokenType::String)
    {
        advance_token();
        Token tok = advance_token();
        auto lit = std::make_shared<LiteralExpr>();
        lit->value = type::Value(type::ValueType::String, PARSER_TRY(canonical_date_string(tok.value)));
        return lit;
    }

    if (check_keyword("timestamp") && peek_token().type == TokenType::String)
    {
        advance_token();
        Token tok = advance_token();
        auto lit = std::make_shared<LiteralExpr>();
        lit->value = type::Value(type::ValueType::String, tok.value);
        return lit;
    }

    if (match_keyword("interval"))
    {
        return PARSER_TRY(parse_interval_expr());
    }

    if (check(TokenType::Integer))
    {
        Token tok = advance_token();
        auto lit = std::make_shared<LiteralExpr>();
        lit->value = type::Value(type::ValueType::Int64, static_cast<int64_t>(PARSER_TRY(base::parse_i64(tok.value))));
        return lit;
    }

    if (check(TokenType::Float))
    {
        Token tok = advance_token();
        auto lit = std::make_shared<LiteralExpr>();
        lit->value = type::Value(type::ValueType::Float64, PARSER_TRY(base::parse_f64(tok.value)));
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
            return PARSER_TRY(maybe_window(PARSER_TRY(parse_function_call(tok.value))));
        }
        if (match(TokenType::Dot))
        {
            if (match(TokenType::Star))
            {
                auto star = std::make_shared<StarExpr>();
                star->table_name = tok.value;
                return star;
            }
            Token col = PARSER_TRY(expect(TokenType::Identifier));
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
        PARSER_TRY(expect(TokenType::LParen));
        auto cast_expr = std::make_shared<CastExpr>();
        cast_expr->operand = PARSER_TRY(parse_expr());
        PARSER_TRY(expect(TokenType::As));
        cast_expr->target = PARSER_TRY(parse_cast_target());
        PARSER_TRY(expect(TokenType::RParen));
        return cast_expr;
    }

    if (check(TokenType::Case))
    {
        return PARSER_TRY(parse_case_expr());
    }

    if (match(TokenType::Exists))
    {
        PARSER_TRY(expect(TokenType::LParen));
        PARSER_TRY(expect(TokenType::Select));
        auto exists = std::make_shared<ExistsExpr>();
        exists->query = PARSER_TRY(parse_select());
        PARSER_TRY(expect(TokenType::RParen));
        return exists;
    }

    if (match(TokenType::LParen))
    {
        if (match(TokenType::Select))
        {
            auto subquery = std::make_shared<SubqueryExpr>();
            subquery->query = PARSER_TRY(parse_select());
            PARSER_TRY(expect(TokenType::RParen));
            return subquery;
        }
        auto expr = PARSER_TRY(parse_expr());
        PARSER_TRY(expect(TokenType::RParen));
        return expr;
    }

    return parse_error(std::format("Unexpected token '{}' in expression", current().value));
}

popo::base::Result<ExprPtr> Parser::parse_case_expr()
{
    PARSER_TRY(expect(TokenType::Case));
    auto case_expr = std::make_shared<CaseExpr>();
    if (!check(TokenType::When))
    {
        case_expr->operand = PARSER_TRY(parse_expr());
    }
    while (match(TokenType::When))
    {
        auto cond = PARSER_TRY(parse_expr());
        PARSER_TRY(expect(TokenType::Then));
        auto result = PARSER_TRY(parse_expr());
        case_expr->whens.emplace_back(cond, result);
    }
    if (case_expr->whens.empty())
    {
        return parse_error("CASE requires at least one WHEN clause");
    }
    if (match(TokenType::Else))
    {
        case_expr->else_expr = PARSER_TRY(parse_expr());
    }
    PARSER_TRY(expect(TokenType::End));
    return case_expr;
}

popo::base::Result<ExprPtr> Parser::parse_comparison_rhs(BinaryOpType op, const ExprPtr & left)
{
    if (check(TokenType::Any) || check(TokenType::All) || check(TokenType::Some))
    {
        bool all = check(TokenType::All);
        advance_token();
        PARSER_TRY(expect(TokenType::LParen));
        PARSER_TRY(expect(TokenType::Select));
        auto quantified = std::make_shared<QuantifiedExpr>();
        quantified->operand = left;
        quantified->op = op;
        quantified->all = all;
        quantified->query = PARSER_TRY(parse_select());
        PARSER_TRY(expect(TokenType::RParen));
        return quantified;
    }
    auto binary = std::make_shared<BinaryOpExpr>();
    binary->op = op;
    binary->left = left;
    binary->right = PARSER_TRY(parse_addition());
    return binary;
}

popo::base::Result<popo::type::ValueType> Parser::parse_cast_target()
{
    int32_t dummy_len = 0;
    return PARSER_TRY(parse_data_type(dummy_len));
}

popo::base::Result<ExprPtr> Parser::maybe_window(ExprPtr func)
{
    if (!match(TokenType::Over))
    {
        return func;
    }
    PARSER_TRY(expect(TokenType::LParen));
    auto window = std::make_shared<WindowExpr>();
    window->function = std::move(func);
    if (match(TokenType::Partition))
    {
        PARSER_TRY(expect(TokenType::By));
        do
        {
            window->partition_by.push_back(PARSER_TRY(parse_expr()));
        } while (match(TokenType::Comma));
    }
    if (match(TokenType::Order))
    {
        PARSER_TRY(expect(TokenType::By));
        window->order_by = PARSER_TRY(parse_order_by());
    }
    PARSER_TRY(expect(TokenType::RParen));
    return window;
}

popo::base::Result<ExprPtr> Parser::parse_function_call(const std::string & name)
{
    PARSER_TRY(expect(TokenType::LParen));

    std::string upper_name;
    for (char c : name)
    {
        upper_name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    if (upper_name == "DATE_ADD" || upper_name == "DATE_SUB")
    {
        auto date_expr = PARSER_TRY(parse_expr());
        PARSER_TRY(expect(TokenType::Comma));
        ExprPtr iv_expr;
        if (match_keyword("interval"))
            iv_expr = PARSER_TRY(parse_interval_expr());
        else
            iv_expr = PARSER_TRY(parse_expr());
        PARSER_TRY(expect(TokenType::RParen));
        auto bin = std::make_shared<BinaryOpExpr>();
        bin->op = upper_name == "DATE_ADD" ? BinaryOpType::Add : BinaryOpType::Sub;
        bin->left = std::move(date_expr);
        bin->right = std::move(iv_expr);
        return bin;
    }

    if (upper_name == "EXTRACT")
    {
        auto call = std::make_shared<ScalarCallExpr>();
        call->name = upper_name;
        if (check(TokenType::Identifier))
        {
            std::string field = current().value;
            for (char & ch : field)
            {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            }
            advance_token();
            PARSER_TRY(expect(TokenType::From));
            auto lit = std::make_shared<LiteralExpr>();
            lit->value = popo::type::Value(popo::type::ValueType::String, field);
            call->args.push_back(lit);
            call->args.push_back(PARSER_TRY(parse_expr()));
        }
        else
        {
            call->args.push_back(PARSER_TRY(parse_expr()));
            PARSER_TRY(expect(TokenType::Comma));
            call->args.push_back(PARSER_TRY(parse_expr()));
        }
        PARSER_TRY(expect(TokenType::RParen));
        return call;
    }

    if (upper_name == "SUBSTRING" || upper_name == "SUBSTR")
    {
        auto call = std::make_shared<ScalarCallExpr>();
        call->name = "SUBSTRING";
        call->args.push_back(PARSER_TRY(parse_expr()));
        if (match(TokenType::From))
        {
            call->args.push_back(PARSER_TRY(parse_expr()));
            if (match(TokenType::For))
            {
                call->args.push_back(PARSER_TRY(parse_expr()));
            }
        }
        else if (match(TokenType::Comma))
        {
            call->args.push_back(PARSER_TRY(parse_expr()));
            if (match(TokenType::Comma))
            {
                call->args.push_back(PARSER_TRY(parse_expr()));
            }
        }
        PARSER_TRY(expect(TokenType::RParen));
        return call;
    }

    if (upper_name == "TRIM")
    {
        auto call = std::make_shared<ScalarCallExpr>();
        call->name = "TRIM";
        std::string mode;
        if (match_keyword("leading"))
            mode = "LEADING";
        else if (match_keyword("trailing"))
            mode = "TRAILING";
        else if (match_keyword("both"))
            mode = "BOTH";

        if (!mode.empty())
        {
            ExprPtr ch_expr;
            if (!match(TokenType::From))
            {
                ch_expr = PARSER_TRY(parse_expr());
                PARSER_TRY(expect(TokenType::From));
            }
            else
            {
                auto space = std::make_shared<LiteralExpr>();
                space->value = popo::type::Value(popo::type::ValueType::String, " ");
                ch_expr = space;
            }
            auto target = PARSER_TRY(parse_expr());
            auto mode_lit = std::make_shared<LiteralExpr>();
            mode_lit->value = popo::type::Value(popo::type::ValueType::String, mode);
            call->args.push_back(target);
            call->args.push_back(mode_lit);
            call->args.push_back(ch_expr);
        }
        else
        {
            auto first = PARSER_TRY(parse_expr());
            if (match(TokenType::From))
            {
                auto target = PARSER_TRY(parse_expr());
                auto mode_lit = std::make_shared<LiteralExpr>();
                mode_lit->value = popo::type::Value(popo::type::ValueType::String, "BOTH");
                call->args.push_back(target);
                call->args.push_back(mode_lit);
                call->args.push_back(first);
            }
            else
            {
                call->args.push_back(first);
            }
        }
        PARSER_TRY(expect(TokenType::RParen));
        return call;
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
        tag->value = popo::type::Value(popo::type::ValueType::String, PARSER_TRY(expect(TokenType::Identifier)).value);
        call->args.push_back(tag);
        if (match(TokenType::Comma))
        {
            call->args.push_back(PARSER_TRY(parse_expr()));
        }
        PARSER_TRY(expect(TokenType::RParen));
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
                auto expr = PARSER_TRY(parse_expr());
                std::string alias;
                if (match(TokenType::As))
                {
                    alias = PARSER_TRY(expect(TokenType::Identifier)).value;
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
        PARSER_TRY(expect(TokenType::RParen));
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
                call->args.push_back(PARSER_TRY(parse_expr()));
            } while (match(TokenType::Comma));
        }
        PARSER_TRY(expect(TokenType::RParen));
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
                func->args.push_back(PARSER_TRY(parse_expr()));
            } while (match(TokenType::Comma));
        }
    }

    PARSER_TRY(expect(TokenType::RParen));
    return func;
}

// CREATE TABLE helpers

popo::base::Result<ColumnDef> Parser::parse_column_def()
{
    Token name = PARSER_TRY(expect(TokenType::Identifier));
    ColumnDef def;
    def.name = name.value;
    def.data_type = PARSER_TRY(parse_data_type(def.type_len));

    while (check(TokenType::Primary) || check(TokenType::Not) || check(TokenType::Null) || check(TokenType::Unique)
           || check(TokenType::Default) || check_keyword("references") || check_keyword("check"))
    {
        if (match(TokenType::Primary))
        {
            PARSER_TRY(expect(TokenType::Key));
            def.is_primary_key = true;
            def.is_nullable = false;
        }
        else if (match(TokenType::Not))
        {
            PARSER_TRY(expect(TokenType::Null));
            def.is_nullable = false;
        }
        else if (match(TokenType::Null))
        {
            def.is_nullable = true;
        }
        else if (match(TokenType::Unique))
        {
            def.is_unique = true;
        }
        else if (check_keyword("references"))
        {
            POPO_TRY(parse_foreign_key_tail());
        }
        else if (match_keyword("check"))
        {
            POPO_TRY(skip_parenthesized());
        }
        else if (match(TokenType::Default))
        {
            auto expr = PARSER_TRY(parse_expr());
            if (expr && expr->type == ExprType::Literal)
            {
                auto * lit = static_cast<LiteralExpr *>(expr.get());
                def.default_value = lit->value;
            }
            else if (expr && expr->type == ExprType::UnaryOp)
            {
                const auto & un = static_cast<const UnaryOpExpr &>(*expr);
                if (un.op == UnaryOpType::Neg && un.operand && un.operand->type == ExprType::Literal)
                {
                    const auto & v = static_cast<const LiteralExpr &>(*un.operand).value;
                    if (v.datatype() == type::ValueType::Float64)
                        def.default_value = type::Value(type::ValueType::Float64, -v.f64());
                    else if (v.datatype() == type::ValueType::Int64)
                        def.default_value = type::Value(type::ValueType::Int64, -v.i64());
                    else
                        return parse_error("DEFAULT value must be a literal");
                }
                else
                {
                    return parse_error("DEFAULT value must be a literal");
                }
            }
            else
            {
                return parse_error("DEFAULT value must be a literal");
            }
        }
    }

    return def;
}

popo::base::Result<popo::type::ValueType> Parser::parse_data_type(int32_t & type_len)
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
        if (match(TokenType::LParen))
        {
            PARSER_TRY(expect(TokenType::Integer));
            PARSER_TRY(expect(TokenType::RParen));
        }
        return type::ValueType::Float32;
    }
    if (match(TokenType::Double))
    {
        match_keyword("precision");
        return type::ValueType::Float64;
    }
    if (match_keyword("real"))
    {
        return type::ValueType::Float64;
    }
    if (match_keyword("decimal") || match_keyword("numeric") || match_keyword("dec"))
    {
        if (match(TokenType::LParen))
        {
            PARSER_TRY(expect(TokenType::Integer));
            if (match(TokenType::Comma))
            {
                PARSER_TRY(expect(TokenType::Integer));
            }
            PARSER_TRY(expect(TokenType::RParen));
        }
        return type::ValueType::Float64;
    }
    if (match_keyword("date") || match_keyword("timestamp") || match_keyword("datetime"))
    {
        if (match(TokenType::LParen))
        {
            PARSER_TRY(expect(TokenType::Integer));
            PARSER_TRY(expect(TokenType::RParen));
        }
        return type::ValueType::String;
    }
    if (match(TokenType::Varchar) || match(TokenType::Char) || match_keyword("character"))
    {
        match_keyword("varying");
        if (match(TokenType::LParen))
        {
            Token len = PARSER_TRY(expect(TokenType::Integer));
            type_len = static_cast<int32_t>(PARSER_TRY(base::parse_i64(len.value)));
            PARSER_TRY(expect(TokenType::RParen));
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
    return parse_error(std::format("Unknown data type: '{}'", current().value));
}

popo::base::Result<StmtPtr> Parser::parse_show()
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
    if (match(TokenType::Grants))
    {
        auto stmt = std::make_shared<ShowGrantsStatement>();
        if (match(TokenType::For))
        {
            stmt->current_user = false;
            stmt->user = PARSER_TRY(parse_account_name());
        }
        return stmt;
    }
    // SHOW without recognized sub-command: treat as show variables (safe fallback)
    return std::make_shared<ShowVariablesStatement>();
}

popo::base::Result<StmtPtr> Parser::parse_describe()
{
    Token table = PARSER_TRY(expect(TokenType::Identifier));
    auto stmt = std::make_shared<DescribeStatement>();
    stmt->table_name = table.value;
    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_set()
{
    const size_t start = pos_;
    auto parsed_assignment = PARSER_TRY(parse_set_assignment());
    if (parsed_assignment)
    {
        return parsed_assignment;
    }
    // Unrecognised SET (SET NAMES utf8mb4, SET sql_mode = ...): accept it as a
    // no-op so MySQL clients can still connect and configure themselves.
    pos_ = start;
    auto stmt = std::make_shared<SetVarStatement>();
    while (!is_at_end() && current().type != TokenType::Semicolon)
    {
        if (!stmt->var_name.empty())
            stmt->var_name += ' ';
        stmt->var_name += advance_token().value;
    }
    return stmt;
}

// Returns nullptr when the tokens are not a SET we understand, leaving pos_ for
// parse_set() to rewind. Malformed recognised input returns an error.
popo::base::Result<StmtPtr> Parser::parse_set_assignment()
{
    TxnScope scope = TxnScope::Session;
    bool explicit_scope = false;
    if (match_keyword("session") || match_keyword("local"))
    {
        explicit_scope = true;
    }
    else if (match_keyword("global"))
    {
        scope = TxnScope::Global;
        explicit_scope = true;
    }

    if (match_keyword("transaction"))
    {
        POPO_TRY(expect_keyword("isolation"));
        POPO_TRY(expect_keyword("level"));
        auto stmt = std::make_shared<SetTransactionStatement>();
        stmt->scope = explicit_scope ? scope : TxnScope::Next;
        stmt->isolation = PARSER_TRY(parse_isolation_level());
        return stmt;
    }

    std::string name;
    if (check(TokenType::SystemVar))
    {
        std::string raw = advance_token().value;
        auto dot = raw.rfind('.');
        if (dot == std::string::npos)
        {
            name = lowercased(raw);
        }
        else
        {
            std::string prefix = lowercased(raw.substr(0, dot));
            if (prefix == "global")
            {
                scope = TxnScope::Global;
            }
            else if (prefix != "session" && prefix != "local")
            {
                return nullptr;
            }
            name = lowercased(raw.substr(dot + 1));
        }
    }
    else if (check(TokenType::Identifier))
    {
        name = lowercased(advance_token().value);
    }
    else
    {
        return nullptr;
    }

    if (!match(TokenType::Eq))
    {
        return nullptr;
    }

    auto stmt = std::make_shared<SetVarStatement>();
    stmt->scope = scope;
    stmt->var_name = name;
    if (check(TokenType::String) || check(TokenType::Integer) || check(TokenType::Float) || check(TokenType::Identifier)
        || check(TokenType::True) || check(TokenType::False) || check(TokenType::Null) || check_keyword("on") || check_keyword("off")
        || check_keyword("default"))
    {
        stmt->value = advance_token().value;
    }
    else
    {
        return parse_error(std::format("Expected a value for SET {}, got '{}'", name, current().value));
    }
    return stmt;
}

popo::base::Result<IsolationLevel> Parser::parse_isolation_level()
{
    if (match_keyword("read"))
    {
        if (match_keyword("uncommitted"))
            return IsolationLevel::ReadUncommitted;
        if (match_keyword("committed"))
            return IsolationLevel::ReadCommitted;
        return parse_error("Expected UNCOMMITTED or COMMITTED after READ");
    }
    if (match_keyword("repeatable"))
    {
        POPO_TRY(expect_keyword("read"));
        return IsolationLevel::RepeatableRead;
    }
    if (match_keyword("serializable"))
    {
        return IsolationLevel::Serializable;
    }
    if (check(TokenType::String))
    {
        std::string text = advance_token().value;
        if (auto level = isolation_level_from_name(text))
        {
            return *level;
        }
        return parse_error(std::format("Unknown isolation level '{}'", text));
    }
    return parse_error(std::format("Unknown isolation level '{}'", current().value));
}

popo::base::Result<StmtPtr> Parser::parse_begin()
{
    advance_token(); // BEGIN
    match_keyword("work");
    match_keyword("transaction");
    auto stmt = std::make_shared<BeginStatement>();
    if (match_keyword("isolation"))
    {
        POPO_TRY(expect_keyword("level"));
        stmt->isolation = PARSER_TRY(parse_isolation_level());
    }
    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_start_transaction()
{
    advance_token(); // START
    POPO_TRY(expect_keyword("transaction"));
    auto stmt = std::make_shared<BeginStatement>();
    if (match_keyword("isolation"))
    {
        POPO_TRY(expect_keyword("level"));
        stmt->isolation = PARSER_TRY(parse_isolation_level());
    }
    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_commit()
{
    advance_token(); // COMMIT
    match_keyword("work");
    match_keyword("transaction");
    return std::make_shared<CommitStatement>();
}

popo::base::Result<StmtPtr> Parser::parse_rollback()
{
    advance_token(); // ROLLBACK
    match_keyword("work");
    match_keyword("transaction");
    return std::make_shared<RollbackStatement>();
}

popo::base::Result<StmtPtr> Parser::parse_select_database()
{
    // SELECT DATABASE()
    advance_token(); // consume DATABASE identifier
    PARSER_TRY(expect(TokenType::LParen));
    PARSER_TRY(expect(TokenType::RParen));
    return std::make_shared<SelectDatabaseStatement>();
}

popo::base::Result<StmtPtr> Parser::parse_select_system_var()
{
    // SELECT @@a [AS alias], @@b [AS alias], ...
    auto stmt = std::make_shared<SelectSystemVarStatement>();
    do
    {
        Token var = PARSER_TRY(expect(TokenType::SystemVar));
        std::string column = var.value;
        auto dot = column.rfind('.');
        if (dot != std::string::npos)
        {
            column = column.substr(dot + 1);
        }
        if (match(TokenType::As))
        {
            column = PARSER_TRY(expect(TokenType::Identifier)).value;
        }
        else if (check(TokenType::Identifier))
        {
            column = advance_token().value;
        }
        stmt->var_names.push_back(var.value);
        stmt->column_names.push_back(column);
    } while (match(TokenType::Comma) && check(TokenType::SystemVar));
    return stmt;
}

popo::base::Result<std::string> Parser::parse_user_token()
{
    if (check(TokenType::String) || check(TokenType::Identifier))
    {
        return advance_token().value;
    }
    return parse_error(std::format("Expected user or host name, got '{}'", current().value));
}

popo::base::Result<AccountName> Parser::parse_account_name()
{
    AccountName account;
    account.user = PARSER_TRY(parse_user_token());
    if (match(TokenType::At))
    {
        if (match(TokenType::Star))
        {
            account.host = "%";
        }
        else
        {
            account.host = PARSER_TRY(parse_user_token());
        }
    }
    if (match(TokenType::Identified))
    {
        if (match(TokenType::With))
        {
            account.plugin = PARSER_TRY(parse_user_token());
        }
        PARSER_TRY(expect(TokenType::By));
        account.password = PARSER_TRY(expect(TokenType::String)).value;
        account.has_password = true;
        if (account.password.find('\n') != std::string::npos)
        {
            return parse_error("password cannot contain a newline");
        }
    }
    return account;
}

popo::base::Result<StmtPtr> Parser::parse_create_user()
{
    auto stmt = std::make_shared<CreateUserStatement>();
    if (match(TokenType::If))
    {
        PARSER_TRY(expect(TokenType::Not));
        PARSER_TRY(expect(TokenType::Exists));
        stmt->if_not_exists = true;
    }
    do
    {
        stmt->users.push_back(PARSER_TRY(parse_account_name()));
    } while (match(TokenType::Comma));
    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_alter_user()
{
    auto stmt = std::make_shared<AlterUserStatement>();
    stmt->user = PARSER_TRY(parse_account_name());
    if (!stmt->user.has_password)
    {
        return parse_error("ALTER USER requires IDENTIFIED BY");
    }
    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_set_password()
{
    PARSER_TRY(expect(TokenType::Password));
    auto stmt = std::make_shared<AlterUserStatement>();
    if (match(TokenType::For))
    {
        stmt->user = PARSER_TRY(parse_account_name());
    }
    PARSER_TRY(expect(TokenType::Eq));
    stmt->user.password = PARSER_TRY(expect(TokenType::String)).value;
    stmt->user.has_password = true;
    if (stmt->user.password.find('\n') != std::string::npos)
    {
        return parse_error("password cannot contain a newline");
    }
    return stmt;
}

popo::base::Result<uint32_t> Parser::parse_privilege_list()
{
    uint32_t privs = 0;
    do
    {
        if (match(TokenType::All))
        {
            match(TokenType::Privileges);
            privs |= kPrivAll;
        }
        else if (match(TokenType::Select))
            privs |= kPrivSelect;
        else if (match(TokenType::Insert))
            privs |= kPrivInsert;
        else if (match(TokenType::Update))
            privs |= kPrivUpdate;
        else if (match(TokenType::Delete))
            privs |= kPrivDelete;
        else if (match(TokenType::Create))
        {
            if (match(TokenType::User))
                privs |= kPrivCreateUser;
            else
                privs |= kPrivCreate;
        }
        else if (match(TokenType::Drop))
            privs |= kPrivDrop;
        else if (match(TokenType::Alter))
            privs |= kPrivAlter;
        else if (match(TokenType::Index))
            privs |= kPrivIndex;
        else if (match(TokenType::Grant))
        {
            PARSER_TRY(expect(TokenType::Option));
            privs |= kPrivGrantOption;
        }
        else if (match(TokenType::Usage))
        {
        }
        else
        {
            return parse_error(std::format("Unknown privilege '{}'", current().value));
        }
    } while (match(TokenType::Comma));
    return privs;
}

popo::base::Result<void> Parser::parse_privilege_level(std::string & database, std::string & table)
{
    if (match(TokenType::Star))
    {
        database = "*";
        table = "*";
        if (match(TokenType::Dot))
        {
            PARSER_TRY(expect(TokenType::Star));
        }
        return {};
    }
    std::string name = PARSER_TRY(parse_user_token());
    if (match(TokenType::Dot))
    {
        database = name;
        if (match(TokenType::Star))
            table = "*";
        else
            table = PARSER_TRY(parse_user_token());
    }
    else
    {
        database.clear();
        table = name;
    }
    return {};
}

popo::base::Result<StmtPtr> Parser::parse_grant()
{
    auto stmt = std::make_shared<GrantStatement>();
    stmt->privs = PARSER_TRY(parse_privilege_list());
    PARSER_TRY(expect(TokenType::On));
    POPO_TRY(parse_privilege_level(stmt->database, stmt->table));
    PARSER_TRY(expect(TokenType::To));
    do
    {
        stmt->users.push_back(PARSER_TRY(parse_account_name()));
    } while (match(TokenType::Comma));
    if (match(TokenType::With))
    {
        PARSER_TRY(expect(TokenType::Grant));
        PARSER_TRY(expect(TokenType::Option));
        stmt->privs |= kPrivGrantOption;
    }
    return stmt;
}

popo::base::Result<StmtPtr> Parser::parse_revoke()
{
    auto stmt = std::make_shared<RevokeStatement>();
    stmt->privs = PARSER_TRY(parse_privilege_list());
    PARSER_TRY(expect(TokenType::On));
    POPO_TRY(parse_privilege_level(stmt->database, stmt->table));
    PARSER_TRY(expect(TokenType::From));
    do
    {
        stmt->users.push_back(PARSER_TRY(parse_account_name()));
    } while (match(TokenType::Comma));
    return stmt;
}

#undef PARSER_TRY

} // namespace popo::parser
