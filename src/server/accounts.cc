#include "server/accounts.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>

namespace popo::server
{
namespace
{

using popo::parser::kPrivAll;
using popo::parser::kPrivAlter;
using popo::parser::kPrivCreate;
using popo::parser::kPrivCreateUser;
using popo::parser::kPrivDelete;
using popo::parser::kPrivDrop;
using popo::parser::kPrivGrantOption;
using popo::parser::kPrivIndex;
using popo::parser::kPrivInsert;
using popo::parser::kPrivSelect;
using popo::parser::kPrivUpdate;

AccountReply ok_reply()
{
    return {};
}

AccountReply fail(uint16_t code, std::string message)
{
    AccountReply reply;
    reply.ok = false;
    reply.code = code;
    reply.message = std::move(message);
    return reply;
}

bool valid_account_token(const std::string & text)
{
    return !text.empty() && text.find_first_of(" \t\r\n") == std::string::npos;
}

std::string quote_sql(const std::string & text)
{
    std::string out;
    out.reserve(text.size());
    for (char ch : text)
    {
        if (ch == '\'')
            out += "''";
        else
            out.push_back(ch);
    }
    return out;
}

bool host_like(const std::string & pattern, const std::string & host)
{
    size_t i = 0;
    size_t j = 0;
    size_t star = std::string::npos;
    size_t match = 0;
    while (j < host.size())
    {
        if (i < pattern.size() && (pattern[i] == host[j] || pattern[i] == '_'))
        {
            ++i;
            ++j;
        }
        else if (i < pattern.size() && pattern[i] == '%')
        {
            star = i++;
            match = j;
        }
        else if (star != std::string::npos)
        {
            i = star + 1;
            j = ++match;
        }
        else
        {
            return false;
        }
    }
    while (i < pattern.size() && pattern[i] == '%')
    {
        ++i;
    }
    return i == pattern.size();
}

int host_score(const std::string & pattern)
{
    bool wild = pattern.find('%') != std::string::npos || pattern.find('_') != std::string::npos;
    if (!wild)
    {
        return 100000 + static_cast<int>(pattern.size());
    }
    int score = 0;
    for (char ch : pattern)
    {
        if (ch != '%' && ch != '_')
            ++score;
    }
    return score;
}

bool scope_covers(
    const std::string & grant_db,
    const std::string & grant_table,
    const std::string & database,
    const std::string & table)
{
    if (grant_db != "*" && grant_db != database)
        return false;
    if (grant_table != "*" && grant_table != table)
        return false;
    return true;
}

std::string encode_privs(uint32_t privs)
{
    if ((privs & kPrivAll) == kPrivAll)
    {
        return (privs & kPrivGrantOption) ? "ALL,GRANT" : "ALL";
    }
    std::string text;
    auto add = [&](uint32_t bit, const char * name)
    {
        if ((privs & bit) == 0)
            return;
        if (!text.empty())
            text.push_back(',');
        text += name;
    };
    add(kPrivSelect, "SELECT");
    add(kPrivInsert, "INSERT");
    add(kPrivUpdate, "UPDATE");
    add(kPrivDelete, "DELETE");
    add(kPrivCreate, "CREATE");
    add(kPrivDrop, "DROP");
    add(kPrivAlter, "ALTER");
    add(kPrivIndex, "INDEX");
    add(kPrivCreateUser, "CREATE_USER");
    add(kPrivGrantOption, "GRANT");
    return text.empty() ? "USAGE" : text;
}

popo::base::Result<uint32_t> decode_privs(const std::string & text)
{
    uint32_t privs = 0;
    std::string token;
    auto apply = [&](std::string name) -> popo::base::Result<void>
    {
        for (char & ch : name)
        {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if (name.empty() || name == "USAGE")
            return {};
        if (name == "ALL")
            privs |= kPrivAll;
        else if (name == "SELECT")
            privs |= kPrivSelect;
        else if (name == "INSERT")
            privs |= kPrivInsert;
        else if (name == "UPDATE")
            privs |= kPrivUpdate;
        else if (name == "DELETE")
            privs |= kPrivDelete;
        else if (name == "CREATE")
            privs |= kPrivCreate;
        else if (name == "DROP")
            privs |= kPrivDrop;
        else if (name == "ALTER")
            privs |= kPrivAlter;
        else if (name == "INDEX")
            privs |= kPrivIndex;
        else if (name == "CREATE_USER")
            privs |= kPrivCreateUser;
        else if (name == "GRANT")
            privs |= kPrivGrantOption;
        else
            return popo::base::make_error(popo::base::InvalidArg, "unknown privilege in account file: " + name);
        return {};
    };
    for (char ch : text)
    {
        if (ch == ',')
        {
            auto applied = apply(token);
            if (!applied)
                return std::unexpected(applied.error());
            token.clear();
        }
        else if (!std::isspace(static_cast<unsigned char>(ch)))
        {
            token.push_back(ch);
        }
    }
    auto applied = apply(token);
    if (!applied)
        return std::unexpected(applied.error());
    return privs;
}

std::string display_privs(uint32_t privs)
{
    if ((privs & kPrivAll) == kPrivAll)
        return "ALL PRIVILEGES";
    std::string text;
    auto add = [&](uint32_t bit, const char * name)
    {
        if ((privs & bit) == 0)
            return;
        if (!text.empty())
            text += ", ";
        text += name;
    };
    add(kPrivSelect, "SELECT");
    add(kPrivInsert, "INSERT");
    add(kPrivUpdate, "UPDATE");
    add(kPrivDelete, "DELETE");
    add(kPrivCreate, "CREATE");
    add(kPrivDrop, "DROP");
    add(kPrivAlter, "ALTER");
    add(kPrivIndex, "INDEX");
    add(kPrivCreateUser, "CREATE USER");
    return text.empty() ? "USAGE" : text;
}

std::string account_label(const std::string & user, const std::string & host)
{
    return "'" + user + "'@'" + host + "'";
}

void resolve_scope(std::string & database, std::string & table, const std::string & current_db)
{
    if (database.empty())
        database = current_db.empty() ? "popo" : current_db;
    if (table.empty())
        table = "*";
}

const char * command_name(popo::parser::StmtType type)
{
    using popo::parser::StmtType;
    switch (type)
    {
        case StmtType::Select:
        case StmtType::ShowTables:
            return "SELECT";
        case StmtType::Insert:
            return "INSERT";
        case StmtType::Update:
            return "UPDATE";
        case StmtType::Delete:
            return "DELETE";
        case StmtType::CreateTable:
        case StmtType::CreateView:
            return "CREATE";
        case StmtType::DropTable:
        case StmtType::DropView:
            return "DROP";
        case StmtType::AlterTable:
            return "ALTER";
        case StmtType::CreateIndex:
            return "INDEX";
        case StmtType::Describe:
            return "DESCRIBE";
        default:
            return "SQL";
    }
}

void collect_tables(
    const popo::parser::TableRef * ref,
    std::vector<std::string> & tables,
    std::vector<const popo::parser::Statement *> & nested)
{
    if (ref == nullptr)
        return;
    if (ref->type == popo::parser::TableRefType::Simple)
    {
        tables.push_back(static_cast<const popo::parser::SimpleTableRef *>(ref)->table_name);
    }
    else if (ref->type == popo::parser::TableRefType::Join)
    {
        const auto * join = static_cast<const popo::parser::JoinTableRef *>(ref);
        collect_tables(join->left.get(), tables, nested);
        collect_tables(join->right.get(), tables, nested);
    }
    else if (ref->type == popo::parser::TableRefType::Derived)
    {
        const auto * derived = static_cast<const popo::parser::DerivedTableRef *>(ref);
        if (derived->query)
            nested.push_back(derived->query.get());
    }
}

} // namespace

std::string normalize_client_host(std::string host)
{
    constexpr std::string_view prefix = "::ffff:";
    if (host.starts_with(prefix))
        host.erase(0, prefix.size());
    return host;
}

bool is_account_statement(popo::parser::StmtType type)
{
    using popo::parser::StmtType;
    return type == StmtType::CreateUser || type == StmtType::DropUser || type == StmtType::AlterUser
        || type == StmtType::Grant || type == StmtType::Revoke || type == StmtType::ShowGrants;
}

AccountStore::AccountStore(std::string datadir, std::string root_password, std::string plugin)
    : datadir_(std::move(datadir))
    , path_(datadir_.empty() ? std::string{} : datadir_ + "/popo_accounts")
    , default_plugin_(plugin.empty() ? "caching_sha2_password" : std::move(plugin))
{
    std::error_code ec;
    const bool account_file_exists = !path_.empty() && std::filesystem::exists(path_, ec);
    if (ec)
    {
        init_status_
            = popo::base::make_error(popo::base::IoErr, std::format("cannot inspect {}: {}", path_, ec.message()));
        spdlog::error("AccountStore::init: {}", init_status_.error().error_message);
        return;
    }
    if (account_file_exists)
    {
        init_status_ = load();
        if (!init_status_)
        {
            spdlog::error("AccountStore::init: {}", init_status_.error().error_message);
            return;
        }
        spdlog::info("Loaded {} account(s) from {}", accounts_.size(), path_);
        return;
    }
    Account root;
    root.user = "root";
    root.host = "%";
    root.plugin = default_plugin_;
    root.password = std::move(root_password);
    root.grants.push_back(GrantEntry{"*", "*", kPrivAll | kPrivGrantOption});
    accounts_.push_back(std::move(root));
    if (path_.empty())
    {
        spdlog::info("Account store is in memory (root@%)");
        return;
    }
    init_status_ = save();
    if (!init_status_)
    {
        spdlog::error("AccountStore::init: {}", init_status_.error().error_message);
        return;
    }
    spdlog::info("Created root@% in {}", path_);
}

std::optional<std::string>
AccountStore::lookup(const std::string & user, const std::string & client_host, std::string & matched_host) const
{
    std::lock_guard lock(mu_);
    if (!init_status_)
        return std::nullopt;
    const Account * best = nullptr;
    int best_score = -1;
    for (const auto & account : accounts_)
    {
        if (account.user != user || !host_like(account.host, client_host))
            continue;
        int score = host_score(account.host);
        if (score > best_score)
        {
            best = &account;
            best_score = score;
        }
    }
    if (best == nullptr)
        return std::nullopt;
    matched_host = best->host;
    return best->password;
}

bool AccountStore::has_priv(
    const std::string & user,
    const std::string & account_host,
    uint32_t need,
    const std::string & database,
    const std::string & table) const
{
    std::lock_guard lock(mu_);
    if (!init_status_)
        return false;
    const Account * account = find_exact(user, account_host);
    if (account == nullptr)
        return false;
    for (const auto & grant : account->grants)
    {
        if ((grant.privs & need) != need)
            continue;
        if (scope_covers(grant.database, grant.table, database, table))
            return true;
    }
    return false;
}

bool AccountStore::can_use_database(
    const std::string & user, const std::string & account_host, const std::string & database) const
{
    std::lock_guard lock(mu_);
    if (!init_status_)
        return false;
    const Account * account = find_exact(user, account_host);
    if (account == nullptr || database.empty())
        return false;
    for (const auto & grant : account->grants)
    {
        if (grant.privs == 0)
            continue;
        if (grant.database == "*" || grant.database == database)
            return true;
    }
    return false;
}

AccountReply AccountStore::execute(const popo::parser::Statement & stmt, const AccountActor & actor)
{
    std::lock_guard lock(mu_);
    if (!init_status_)
        return fail(1, init_status_.error().error_message);
    return execute_unlocked(stmt, actor);
}

AccountReply AccountStore::authorize(const popo::parser::Statement & stmt, const AccountActor & actor) const
{
    std::lock_guard lock(mu_);
    if (!init_status_)
        return fail(1, init_status_.error().error_message);
    return authorize_unlocked(stmt, actor);
}

AccountStore::Account * AccountStore::find_exact(const std::string & user, const std::string & host)
{
    for (auto & account : accounts_)
    {
        if (account.user == user && account.host == host)
            return &account;
    }
    return nullptr;
}

const AccountStore::Account * AccountStore::find_exact(const std::string & user, const std::string & host) const
{
    for (const auto & account : accounts_)
    {
        if (account.user == user && account.host == host)
            return &account;
    }
    return nullptr;
}

popo::base::Result<void> AccountStore::load()
{
    std::ifstream in(path_);
    if (!in)
        return popo::base::make_error(popo::base::IoErr, "cannot read " + path_);
    accounts_.clear();
    Account * current = nullptr;
    std::string line;
    int line_no = 0;
    auto malformed = [&](std::string message)
    {
        return popo::base::make_error(
            popo::base::InvalidArg, std::format("{}:{}: {}", path_, line_no, std::move(message)));
    };
    while (std::getline(in, line))
    {
        ++line_no;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (line.starts_with("account "))
        {
            std::istringstream fields(line.substr(8));
            Account account;
            if (!(fields >> account.user >> account.host >> account.plugin))
                return malformed("expected user host plugin");
            accounts_.push_back(std::move(account));
            current = &accounts_.back();
        }
        else if (line.starts_with("password"))
        {
            if (current == nullptr)
                return malformed("password before account");
            current->password = line.size() > 9 ? line.substr(9) : std::string{};
        }
        else if (line.starts_with("grant "))
        {
            if (current == nullptr)
                return malformed("grant before account");
            std::string rest = line.substr(6);
            auto first = rest.find(' ');
            auto second = first == std::string::npos ? std::string::npos : rest.find(' ', first + 1);
            if (first == std::string::npos || second == std::string::npos)
                return malformed("expected db table privs");
            auto privs = decode_privs(rest.substr(second + 1));
            if (!privs)
                return malformed(privs.error().error_message);
            current->grants.push_back(
                GrantEntry{rest.substr(0, first), rest.substr(first + 1, second - first - 1), *privs});
        }
        else
            return malformed("unrecognized line");
    }
    if (in.bad())
        return popo::base::make_error(popo::base::IoErr, "failed while reading " + path_);
    if (accounts_.empty())
        return popo::base::make_error(popo::base::InvalidArg, path_ + " has no accounts");
    return {};
}

popo::base::Result<void> AccountStore::save()
{
    if (path_.empty())
        return {};
    std::error_code ec;
    std::filesystem::create_directories(datadir_, ec);
    if (ec)
        return popo::base::make_error(popo::base::IoErr, std::format("cannot create {}: {}", datadir_, ec.message()));
    auto tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return popo::base::make_error(popo::base::IoErr, "cannot write " + tmp);
        out << "# PopoDB accounts. Passwords are plaintext.\n";
        for (const auto & account : accounts_)
        {
            out << "account " << account.user << ' ' << account.host << ' ' << account.plugin << '\n';
            out << "password " << account.password << '\n';
            for (const auto & grant : account.grants)
            {
                if (grant.privs == 0)
                    continue;
                out << "grant " << grant.database << ' ' << grant.table << ' ' << encode_privs(grant.privs) << '\n';
            }
        }
        out.flush();
        if (!out)
            return popo::base::make_error(popo::base::IoErr, "failed to write " + tmp);
    }
    std::filesystem::rename(tmp, path_, ec);
    if (ec)
        return popo::base::make_error(popo::base::IoErr, std::format("cannot replace {}: {}", path_, ec.message()));
    return {};
}

AccountReply AccountStore::execute_unlocked(const popo::parser::Statement & stmt, const AccountActor & actor)
{
    const Account * actor_account = find_exact(actor.user, actor.host);
    if (actor_account == nullptr)
        return fail(1045, "Access denied for user " + account_label(actor.user, actor.host));

    auto has_create_user = [&]()
    {
        return std::any_of(
            actor_account->grants.begin(),
            actor_account->grants.end(),
            [](const GrantEntry & grant) { return (grant.privs & kPrivCreateUser) != 0; });
    };
    auto has_grant_option = [&]()
    {
        return std::any_of(
            actor_account->grants.begin(),
            actor_account->grants.end(),
            [](const GrantEntry & grant) { return (grant.privs & kPrivGrantOption) != 0; });
    };
    auto can_change = [&](const popo::parser::AccountName & target)
    {
        if (target.user == actor.user && target.host == actor.host)
            return true;
        return has_create_user();
    };
    auto can_admin = [&](uint32_t privs, const std::string & database, const std::string & table)
    {
        uint32_t want = privs & ~kPrivGrantOption;
        for (const auto & grant : actor_account->grants)
        {
            if ((grant.privs & kPrivGrantOption) == 0)
                continue;
            if (!scope_covers(grant.database, grant.table, database, table))
                continue;
            if ((grant.privs & want) != want)
                continue;
            return true;
        }
        return false;
    };
    auto check_name = [&](const popo::parser::AccountName & name) -> AccountReply
    {
        if (!valid_account_token(name.user) || !valid_account_token(name.host))
            return fail(1396, "Operation CREATE USER failed for " + account_label(name.user, name.host));
        if (name.password.find('\n') != std::string::npos)
            return fail(1396, "password cannot contain a newline");
        return ok_reply();
    };

    using popo::parser::StmtType;
    if (stmt.type == StmtType::CreateUser)
    {
        if (!has_create_user())
            return fail(1227, "Access denied; you need the CREATE USER privilege for this operation");
        const auto & create = static_cast<const popo::parser::CreateUserStatement &>(stmt);
        for (const auto & user : create.users)
        {
            if (auto bad = check_name(user); !bad.ok)
                return bad;
            if (find_exact(user.user, user.host) != nullptr && !create.if_not_exists)
                return fail(1396, "Operation CREATE USER failed for " + account_label(user.user, user.host));
        }
        for (const auto & user : create.users)
        {
            if (find_exact(user.user, user.host) != nullptr)
                continue;
            Account created;
            created.user = user.user;
            created.host = user.host;
            created.plugin = user.plugin.empty() ? default_plugin_ : user.plugin;
            created.password = user.has_password ? user.password : std::string{};
            accounts_.push_back(std::move(created));
        }
        auto saved = save();
        if (!saved)
            return fail(1, saved.error().error_message);
        return ok_reply();
    }
    if (stmt.type == StmtType::DropUser)
    {
        if (!has_create_user())
            return fail(1227, "Access denied; you need the CREATE USER privilege for this operation");
        const auto & drop = static_cast<const popo::parser::DropUserStatement &>(stmt);
        for (const auto & user : drop.users)
        {
            if (user.user == actor.user && user.host == actor.host)
                return fail(1396, "cannot drop the current account " + account_label(user.user, user.host));
            if (find_exact(user.user, user.host) == nullptr && !drop.if_exists)
                return fail(1396, "Operation DROP USER failed for " + account_label(user.user, user.host));
        }
        for (const auto & user : drop.users)
        {
            std::erase_if(
                accounts_,
                [&](const Account & account) { return account.user == user.user && account.host == user.host; });
        }
        auto saved = save();
        if (!saved)
            return fail(1, saved.error().error_message);
        return ok_reply();
    }
    if (stmt.type == StmtType::AlterUser)
    {
        auto user = static_cast<const popo::parser::AlterUserStatement &>(stmt).user;
        if (user.user.empty())
        {
            user.user = actor.user;
            user.host = actor.host;
        }
        if (!can_change(user))
            return fail(1227, "Access denied; you need the CREATE USER privilege for this operation");
        Account * account = find_exact(user.user, user.host);
        if (account == nullptr)
            return fail(1396, "Operation ALTER USER failed for " + account_label(user.user, user.host));
        if (user.password.find('\n') != std::string::npos)
            return fail(1396, "password cannot contain a newline");
        account->password = user.password;
        if (!user.plugin.empty())
            account->plugin = user.plugin;
        auto saved = save();
        if (!saved)
            return fail(1, saved.error().error_message);
        return ok_reply();
    }
    if (stmt.type == StmtType::Grant || stmt.type == StmtType::Revoke)
    {
        uint32_t privs = 0;
        std::string database;
        std::string table;
        const std::vector<popo::parser::AccountName> * users = nullptr;
        if (stmt.type == StmtType::Grant)
        {
            const auto & grant = static_cast<const popo::parser::GrantStatement &>(stmt);
            privs = grant.privs;
            database = grant.database;
            table = grant.table;
            users = &grant.users;
        }
        else
        {
            const auto & revoke = static_cast<const popo::parser::RevokeStatement &>(stmt);
            privs = revoke.privs;
            database = revoke.database;
            table = revoke.table;
            users = &revoke.users;
        }
        resolve_scope(database, table, actor.database);
        if (!can_admin(privs, database, table))
            return fail(1227, "Access denied; you need the GRANT OPTION privilege for this operation");
        for (const auto & user : *users)
        {
            if (find_exact(user.user, user.host) == nullptr)
                return fail(
                    1133,
                    "Can't find any matching row in the user table for the user "
                        + account_label(user.user, user.host));
        }
        for (const auto & user : *users)
        {
            Account * account = find_exact(user.user, user.host);
            if (stmt.type == StmtType::Grant)
            {
                if (privs == 0)
                    continue;
                auto it = std::find_if(
                    account->grants.begin(),
                    account->grants.end(),
                    [&](const GrantEntry & grant) { return grant.database == database && grant.table == table; });
                if (it == account->grants.end())
                    account->grants.push_back(GrantEntry{database, table, privs});
                else
                    it->privs |= privs;
            }
            else
            {
                for (auto & grant : account->grants)
                {
                    if (grant.database == database && grant.table == table)
                        grant.privs &= ~privs;
                }
                std::erase_if(account->grants, [](const GrantEntry & grant) { return grant.privs == 0; });
            }
        }
        auto saved = save();
        if (!saved)
            return fail(1, saved.error().error_message);
        return ok_reply();
    }
    if (stmt.type == StmtType::ShowGrants)
    {
        auto who = static_cast<const popo::parser::ShowGrantsStatement &>(stmt).user;
        bool current = static_cast<const popo::parser::ShowGrantsStatement &>(stmt).current_user || who.user.empty();
        if (current)
        {
            who.user = actor.user;
            who.host = actor.host;
        }
        bool self = who.user == actor.user && who.host == actor.host;
        if (!self && !has_create_user() && !has_grant_option())
            return fail(1227, "Access denied; you need the CREATE USER privilege for this operation");
        const Account * account = find_exact(who.user, who.host);
        if (account == nullptr)
            return fail(1141, "There is no such grant defined for user " + account_label(who.user, who.host));
        AccountReply reply;
        reply.column = "Grants for " + who.user + "@" + who.host;
        if (account->grants.empty())
        {
            reply.rows.push_back("GRANT USAGE ON *.* TO " + account_label(account->user, account->host));
            return reply;
        }
        for (const auto & grant : account->grants)
        {
            std::string line = "GRANT " + display_privs(grant.privs) + " ON " + grant.database + "." + grant.table
                + " TO " + account_label(account->user, account->host);
            if ((grant.privs & kPrivGrantOption) != 0)
                line += " WITH GRANT OPTION";
            reply.rows.push_back(std::move(line));
        }
        return reply;
    }
    return fail(1064, "account statement is not supported");
}

AccountReply AccountStore::authorize_unlocked(const popo::parser::Statement & stmt, const AccountActor & actor) const
{
    if (is_account_statement(stmt.type))
        return ok_reply();
    const Account * account = find_exact(actor.user, actor.host);
    if (account == nullptr)
        return fail(1045, "Access denied for user " + account_label(actor.user, actor.client_host));

    auto denied = [&](const std::string & table)
    {
        return fail(
            1142,
            std::format(
                "{} command denied to user '{}'@'{}' for table '{}'",
                command_name(stmt.type),
                actor.user,
                actor.client_host,
                table));
    };
    auto need_table = [&](uint32_t priv, const std::string & table) -> AccountReply
    {
        for (const auto & grant : account->grants)
        {
            if ((grant.privs & priv) == priv && scope_covers(grant.database, grant.table, actor.database, table))
                return ok_reply();
        }
        return denied(table);
    };
    auto check_one = [&](const popo::parser::Statement & one, auto && self) -> AccountReply
    {
        using popo::parser::StmtType;
        switch (one.type)
        {
            case StmtType::ShowDatabases:
            case StmtType::ShowVariables:
            case StmtType::ShowStatus:
            case StmtType::SetVar:
            case StmtType::SelectDatabase:
            case StmtType::SelectSystemVar:
                return ok_reply();
            case StmtType::ShowTables: {
                for (const auto & grant : account->grants)
                {
                    if ((grant.privs & kPrivSelect) != 0 && (grant.database == "*" || grant.database == actor.database))
                        return ok_reply();
                }
                return fail(
                    1044,
                    std::format(
                        "Access denied for user '{}'@'{}' to database '{}'",
                        actor.user,
                        actor.client_host,
                        actor.database));
            }
            case StmtType::Describe:
                return need_table(kPrivSelect, static_cast<const popo::parser::DescribeStatement &>(one).table_name);
            case StmtType::Insert:
                return need_table(kPrivInsert, static_cast<const popo::parser::InsertStatement &>(one).table_name);
            case StmtType::Update:
                return need_table(kPrivUpdate, static_cast<const popo::parser::UpdateStatement &>(one).table_name);
            case StmtType::Delete:
                return need_table(kPrivDelete, static_cast<const popo::parser::DeleteStatement &>(one).table_name);
            case StmtType::CreateTable:
                return need_table(kPrivCreate, static_cast<const popo::parser::CreateTableStatement &>(one).table_name);
            case StmtType::DropTable:
                return need_table(kPrivDrop, static_cast<const popo::parser::DropTableStatement &>(one).table_name);
            case StmtType::AlterTable:
                return need_table(kPrivAlter, static_cast<const popo::parser::AlterTableStatement &>(one).table_name);
            case StmtType::CreateIndex:
                return need_table(kPrivIndex, static_cast<const popo::parser::CreateIndexStatement &>(one).table_name);
            case StmtType::CreateView:
                return need_table(kPrivCreate, static_cast<const popo::parser::CreateViewStatement &>(one).name);
            case StmtType::DropView:
                return need_table(kPrivDrop, static_cast<const popo::parser::DropViewStatement &>(one).name);
            case StmtType::Select: {
                const auto & select = static_cast<const popo::parser::SelectStatement &>(one);
                for (const auto & cte : select.with_clause)
                {
                    if (cte.query)
                    {
                        if (auto reply = self(*cte.query, self); !reply.ok)
                            return reply;
                    }
                }
                for (const auto & term : select.set_ops)
                {
                    if (term.query)
                    {
                        if (auto reply = self(*term.query, self); !reply.ok)
                            return reply;
                    }
                }
                std::vector<std::string> tables;
                std::vector<const popo::parser::Statement *> nested;
                collect_tables(select.from.get(), tables, nested);
                for (const auto & table : tables)
                {
                    bool is_cte = std::any_of(
                        select.with_clause.begin(),
                        select.with_clause.end(),
                        [&](const popo::parser::CteDef & cte)
                        {
                            if (cte.name.size() != table.size())
                                return false;
                            for (size_t k = 0; k < table.size(); ++k)
                            {
                                if (std::tolower(static_cast<unsigned char>(cte.name[k]))
                                    != std::tolower(static_cast<unsigned char>(table[k])))
                                    return false;
                            }
                            return true;
                        });
                    if (is_cte)
                        continue;
                    if (auto reply = need_table(kPrivSelect, table); !reply.ok)
                        return reply;
                }
                for (const auto * nested_stmt : nested)
                {
                    if (auto reply = self(*nested_stmt, self); !reply.ok)
                        return reply;
                }
                return ok_reply();
            }
            default:
                return denied("*");
        }
    };
    return check_one(stmt, check_one);
}

} // namespace popo::server
