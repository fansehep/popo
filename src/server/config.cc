#include "server/config.h"

#include <cctype>
#include <fstream>

namespace popo::server
{
namespace
{

std::string trim(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }
    if (text.size() >= 2
        && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\'')))
    {
        text = text.substr(1, text.size() - 2);
    }
    return text;
}

bool truthy(const std::string & value)
{
    return value == "1" || value == "true" || value == "on" || value == "yes";
}

} // namespace

popo::base::Result<ServerConfig> load_server_config(const std::string & path)
{
    std::ifstream in(path);
    if (!in)
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "cannot read defaults file " + path);
    }
    ServerConfig config;
    std::string line;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
        {
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        for (char & ch : key)
        {
            if (ch == '_')
                ch = '-';
        }
        if (key == "port")
        {
            auto port = popo::base::parse_u16(value);
            if (!port)
            {
                return popo::base::make_error(
                    popo::base::Errno::InvalidArg,
                    "invalid port in defaults file " + path + ": " + port.error().error_message);
            }
            config.port = *port;
        }
        else if (key == "bind-address")
            config.bind_address = value;
        else if (key == "datadir")
            config.datadir = value;
        else if (key == "database")
            config.database = value;
        else if (key == "default-auth-plugin")
            config.plugin = value;
        else if (key == "root-password")
            config.root_password = value;
        else if (key == "require-full-auth")
            config.require_full_auth = truthy(value);
    }
    if (in.bad())
    {
        return popo::base::make_error(popo::base::Errno::IoErr, "error reading defaults file " + path);
    }
    return config;
}

} // namespace popo::server
