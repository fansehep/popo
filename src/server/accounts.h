#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "base/error.h"
#include "parser/ast.h"

namespace popo::server
{

struct AccountActor
{
    std::string user;
    std::string host;
    std::string client_host;
    std::string database;
};

struct AccountReply
{
    bool ok = true;
    uint16_t code = 0;
    std::string message;
    std::string column;
    std::vector<std::string> rows;
};

// Accounts live in {datadir}/popo_accounts. An empty datadir stays in memory.
// Passwords are plaintext because caching_sha2 full auth compares the decrypted password.
class AccountStore
{
public:
    AccountStore(std::string datadir, std::string root_password, std::string plugin);

    const popo::base::Result<void> & status() const { return init_status_; }
    std::optional<std::string>
    lookup(const std::string & user, const std::string & client_host, std::string & matched_host) const;
    bool has_priv(
        const std::string & user,
        const std::string & account_host,
        uint32_t need,
        const std::string & database,
        const std::string & table) const;
    bool
    can_use_database(const std::string & user, const std::string & account_host, const std::string & database) const;

    AccountReply execute(const popo::parser::Statement & stmt, const AccountActor & actor);
    AccountReply authorize(const popo::parser::Statement & stmt, const AccountActor & actor) const;

private:
    struct GrantEntry
    {
        std::string database;
        std::string table;
        uint32_t privs = 0;
    };
    struct Account
    {
        std::string user;
        std::string host;
        std::string plugin;
        std::string password;
        std::vector<GrantEntry> grants;
    };

    Account * find_exact(const std::string & user, const std::string & host);
    const Account * find_exact(const std::string & user, const std::string & host) const;
    popo::base::Result<void> load();
    popo::base::Result<void> save();
    AccountReply execute_unlocked(const popo::parser::Statement & stmt, const AccountActor & actor);
    AccountReply authorize_unlocked(const popo::parser::Statement & stmt, const AccountActor & actor) const;

    std::string datadir_;
    std::string path_;
    std::string default_plugin_;
    std::vector<Account> accounts_;
    popo::base::Result<void> init_status_;
    mutable std::mutex mu_;
};

std::string normalize_client_host(std::string host);
bool is_account_statement(popo::parser::StmtType type);

} // namespace popo::server
