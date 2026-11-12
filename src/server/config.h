#pragma once

#include "base/error.h"

#include <cstdint>
#include <string>

namespace popo::server
{

struct ServerConfig
{
    uint16_t port = 3307;
    std::string bind_address = "0.0.0.0";
    std::string datadir = "data";
    std::string database = "popo";
    std::string plugin = "caching_sha2_password";
    std::string root_password;
    bool require_full_auth = false;
};

// Read a MySQL-style ini file. Unknown keys are ignored.
popo::base::Result<ServerConfig> load_server_config(const std::string & path);

} // namespace popo::server
