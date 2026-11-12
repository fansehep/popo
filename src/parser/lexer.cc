#include "parser/lexer.h"

#include <cctype>
#include <unordered_map>

namespace popo::parser
{

Lexer::Lexer(const std::string & input) : input_(input)
{
}

char Lexer::peek() const
{
    if (pos_ >= input_.size())
    {
        return '\0';
    }
    return input_[pos_];
}

char Lexer::advance()
{
    char c = peek();
    if (c == '\n')
    {
        line_++;
        col_ = 1;
    }
    else
    {
        col_++;
    }
    pos_++;
    return c;
}

void Lexer::skip_whitespace()
{
    while (std::isspace(static_cast<unsigned char>(peek())))
    {
        advance();
    }
}

void Lexer::skip_line_comment()
{
    while (peek() != '\0' && peek() != '\n')
    {
        advance();
    }
}

void Lexer::skip_block_comment()
{
    advance(); // '/'
    advance(); // '*'
    while (peek() != '\0')
    {
        if (peek() == '*' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '/')
        {
            advance(); // '*'
            advance(); // '/'
            return;
        }
        advance();
    }
}

Token Lexer::read_number()
{
    int start_line = line_;
    int start_col = col_;
    std::string value;
    bool is_float = false;

    if (peek() == '.')
    {
        is_float = true;
        value.push_back(advance());
    }

    while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.')
    {
        if (peek() == '.')
        {
            if (is_float)
            {
                break;
            }
            is_float = true;
        }
        value.push_back(advance());
    }

    if (peek() == 'e' || peek() == 'E')
    {
        size_t next = pos_ + 1;
        bool valid_exp = false;
        if (next < input_.size() && std::isdigit(static_cast<unsigned char>(input_[next])))
        {
            valid_exp = true;
        }
        else if (
            next + 1 < input_.size() && (input_[next] == '+' || input_[next] == '-')
            && std::isdigit(static_cast<unsigned char>(input_[next + 1])))
        {
            valid_exp = true;
        }
        if (valid_exp)
        {
            is_float = true;
            value.push_back(advance()); // e / E
            if (peek() == '+' || peek() == '-')
            {
                value.push_back(advance());
            }
            while (std::isdigit(static_cast<unsigned char>(peek())))
            {
                value.push_back(advance());
            }
        }
    }

    TokenType type = is_float ? TokenType::Float : TokenType::Integer;
    return Token{type, value, start_line, start_col};
}

Token Lexer::read_string()
{
    int start_line = line_;
    int start_col = col_;
    char quote = advance(); // ' or "
    std::string value;

    while (peek() != '\0')
    {
        if (peek() == quote)
        {
            advance(); // closing or escaped quote
            if (peek() == quote)
            {
                value.push_back(advance());
                continue;
            }
            break;
        }
        value.push_back(advance());
    }

    return Token{TokenType::String, value, start_line, start_col};
}

Token Lexer::read_identifier_or_keyword()
{
    int start_line = line_;
    int start_col = col_;
    std::string value;

    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')
    {
        value.push_back(advance());
    }

    std::string lower;
    lower.reserve(value.size());
    for (char c : value)
    {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    TokenType type = keyword_lookup(lower);
    if (type != TokenType::Identifier)
    {
        return Token{type, value, start_line, start_col};
    }
    return Token{TokenType::Identifier, value, start_line, start_col};
}

TokenType Lexer::keyword_lookup(const std::string & word)
{
    static const std::unordered_map<std::string, TokenType> keywords = {
        {"select", TokenType::Select},
        {"from", TokenType::From},
        {"where", TokenType::Where},
        {"insert", TokenType::Insert},
        {"into", TokenType::Into},
        {"values", TokenType::Values},
        {"create", TokenType::Create},
        {"table", TokenType::Table},
        {"index", TokenType::Index},
        {"drop", TokenType::Drop},
        {"update", TokenType::Update},
        {"set", TokenType::Set},
        {"delete", TokenType::Delete},
        {"alter", TokenType::Alter},
        {"and", TokenType::And},
        {"or", TokenType::Or},
        {"not", TokenType::Not},
        {"null", TokenType::Null},
        {"true", TokenType::True},
        {"false", TokenType::False},
        {"join", TokenType::Join},
        {"inner", TokenType::Inner},
        {"left", TokenType::Left},
        {"right", TokenType::Right},
        {"cross", TokenType::Cross},
        {"on", TokenType::On},
        {"group", TokenType::Group},
        {"by", TokenType::By},
        {"having", TokenType::Having},
        {"order", TokenType::Order},
        {"asc", TokenType::Asc},
        {"desc", TokenType::Desc},
        {"limit", TokenType::Limit},
        {"as", TokenType::As},
        {"primary", TokenType::Primary},
        {"key", TokenType::Key},
        {"int", TokenType::Int},
        {"integer", TokenType::Integer_},
        {"bigint", TokenType::Bigint},
        {"smallint", TokenType::Smallint},
        {"tinyint", TokenType::Tinyint},
        {"float", TokenType::Float_},
        {"double", TokenType::Double},
        {"varchar", TokenType::Varchar},
        {"char", TokenType::Char},
        {"text", TokenType::Text},
        {"boolean", TokenType::Boolean},
        {"bool", TokenType::Bool_},
        {"unique", TokenType::Unique},
        {"default", TokenType::Default},
        {"if", TokenType::If},
        {"exists", TokenType::Exists},
        {"count", TokenType::Count},
        {"sum", TokenType::Sum},
        {"avg", TokenType::Avg},
        {"min", TokenType::Min},
        {"max", TokenType::Max},
        {"distinct", TokenType::Distinct},
        {"in", TokenType::In},
        {"between", TokenType::Between},
        {"like", TokenType::Like},
        {"is", TokenType::Is},
        {"case", TokenType::Case},
        {"when", TokenType::When},
        {"then", TokenType::Then},
        {"else", TokenType::Else},
        {"end", TokenType::End},
        {"union", TokenType::Union},
        {"intersect", TokenType::Intersect},
        {"except", TokenType::Except},
        {"all", TokenType::All},
        {"any", TokenType::Any},
        {"some", TokenType::Some},
        {"full", TokenType::Full},
        {"outer", TokenType::Outer},
        {"recursive", TokenType::Recursive},
        {"cast", TokenType::Cast},
        {"over", TokenType::Over},
        {"partition", TokenType::Partition},
        {"view", TokenType::View},
        {"user", TokenType::User},
        {"identified", TokenType::Identified},
        {"grant", TokenType::Grant},
        {"grants", TokenType::Grants},
        {"revoke", TokenType::Revoke},
        {"privileges", TokenType::Privileges},
        {"option", TokenType::Option},
        {"usage", TokenType::Usage},
        {"password", TokenType::Password},
        {"for", TokenType::For},
        {"offset", TokenType::Offset},
        {"with", TokenType::With},
        {"add", TokenType::Add},
        {"column", TokenType::Column},
        {"rename", TokenType::Rename},
        {"to", TokenType::To},
        {"show", TokenType::Show},
        {"databases", TokenType::Databases},
        {"describe", TokenType::Describe},
        {"tables", TokenType::Tables},
        {"variables", TokenType::Variables},
        {"status", TokenType::Status},
    };

    auto it = keywords.find(word);
    if (it != keywords.end())
    {
        return it->second;
    }
    return TokenType::Identifier;
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;

    while (peek() != '\0')
    {
        skip_whitespace();

        if (peek() == '\0')
        {
            break;
        }

        char c = peek();
        int start_line = line_;
        int start_col = col_;

        // Line comment
        if (c == '-' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '-')
        {
            skip_line_comment();
            continue;
        }

        // Block comment
        if (c == '/' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '*')
        {
            skip_block_comment();
            continue;
        }

        // String literals
        if (c == '\'' || c == '"')
        {
            tokens.push_back(read_string());
            continue;
        }

        // Numbers (including leading-dot floats like .06)
        if (std::isdigit(static_cast<unsigned char>(c))
            || (c == '.' && pos_ + 1 < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_ + 1]))
                && (tokens.empty() || tokens.back().type != TokenType::Identifier)))
        {
            tokens.push_back(read_number());
            continue;
        }

        // Backtick-quoted identifiers (`col`)
        if (c == '`')
        {
            advance();
            std::string ident;
            while (peek() != '\0' && peek() != '`')
            {
                ident.push_back(advance());
            }
            if (peek() == '`')
            {
                advance();
            }
            tokens.push_back(Token{TokenType::Identifier, ident, start_line, start_col});
            continue;
        }

        // System variables (@@xxx)
        if (c == '@' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '@')
        {
            advance(); // first @
            advance(); // second @
            start_line = line_;
            start_col = col_;
            std::string var_name;
            while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_' || peek() == '.')
            {
                var_name.push_back(advance());
            }
            tokens.push_back(Token{TokenType::SystemVar, var_name, start_line, start_col});
            continue;
        }

        // Identifiers and keywords
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            tokens.push_back(read_identifier_or_keyword());
            continue;
        }

        // Multi-char operators
        if (c == '!' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=')
        {
            advance();
            advance();
            tokens.push_back(Token{TokenType::Ne, "!=", start_line, start_col});
            continue;
        }
        if (c == '<' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '>')
        {
            advance();
            advance();
            tokens.push_back(Token{TokenType::Ne, "<>", start_line, start_col});
            continue;
        }
        if (c == '<' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=')
        {
            advance();
            advance();
            tokens.push_back(Token{TokenType::Le, "<=", start_line, start_col});
            continue;
        }
        if (c == '>' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '=')
        {
            advance();
            advance();
            tokens.push_back(Token{TokenType::Ge, ">=", start_line, start_col});
            continue;
        }
        if (c == '|' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '|')
        {
            advance();
            advance();
            tokens.push_back(Token{TokenType::Concat, "||", start_line, start_col});
            continue;
        }

        // Single-char operators and delimiters
        advance();
        switch (c)
        {
            case '+':
                tokens.push_back(Token{TokenType::Plus, "+", start_line, start_col});
                break;
            case '-':
                tokens.push_back(Token{TokenType::Minus, "-", start_line, start_col});
                break;
            case '*':
                tokens.push_back(Token{TokenType::Star, "*", start_line, start_col});
                break;
            case '/':
                tokens.push_back(Token{TokenType::Slash, "/", start_line, start_col});
                break;
            case '=':
                tokens.push_back(Token{TokenType::Eq, "=", start_line, start_col});
                break;
            case '<':
                tokens.push_back(Token{TokenType::Lt, "<", start_line, start_col});
                break;
            case '>':
                tokens.push_back(Token{TokenType::Gt, ">", start_line, start_col});
                break;
            case '(':
                tokens.push_back(Token{TokenType::LParen, "(", start_line, start_col});
                break;
            case ')':
                tokens.push_back(Token{TokenType::RParen, ")", start_line, start_col});
                break;
            case ',':
                tokens.push_back(Token{TokenType::Comma, ",", start_line, start_col});
                break;
            case ';':
                tokens.push_back(Token{TokenType::Semicolon, ";", start_line, start_col});
                break;
            case '.':
                tokens.push_back(Token{TokenType::Dot, ".", start_line, start_col});
                break;
            case '@':
                tokens.push_back(Token{TokenType::At, "@", start_line, start_col});
                break;
            default:
                tokens.push_back(Token{TokenType::Invalid, std::string(1, c), start_line, start_col});
                break;
        }
    }

    tokens.push_back(Token{TokenType::Eof, "", line_, col_});
    return tokens;
}

} // namespace popo::parser
