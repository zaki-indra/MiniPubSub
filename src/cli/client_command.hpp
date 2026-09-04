#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mps::cli
{

enum class CommandKind { ping, publish, subscribe, unsubscribe, help, exit };

struct Command {
    CommandKind kind;
    std::vector<std::string> arguments;
};

struct ParseResult {
    bool empty{false};
    bool valid{false};
    Command command{CommandKind::help, {}};
};

ParseResult parse_command(std::string_view line);
std::string escape_bytes(std::string_view bytes, bool quote);

} // namespace mps::cli
