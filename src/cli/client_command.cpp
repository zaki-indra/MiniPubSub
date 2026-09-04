#include "cli/client_command.hpp"

#include <cctype>
#include <cstdint>

namespace mps::cli
{
namespace
{

int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool append_escape(std::string_view line, std::size_t& index, std::string& token) {
    if (++index >= line.size()) {
        return false;
    }
    const char escaped = line[index];
    switch (escaped) {
    case 'n': token.push_back('\n'); break;
    case 'r': token.push_back('\r'); break;
    case 't': token.push_back('\t'); break;
    case '0': token.push_back('\0'); break;
    case 'x': {
        if (index + 2U >= line.size()) {
            return false;
        }
        const int high = hex_value(line[index + 1U]);
        const int low = hex_value(line[index + 2U]);
        if (high < 0 || low < 0) {
            return false;
        }
        token.push_back(static_cast<char>((high << 4) | low));
        index += 2U;
        break;
    }
    default: token.push_back(escaped); break;
    }
    return true;
}

bool tokenize(std::string_view line, std::vector<std::string>& tokens) {
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() &&
               std::isspace(static_cast<unsigned char>(line[index])) != 0) {
            ++index;
        }
        if (index == line.size()) {
            break;
        }
        std::string token;
        bool started = false;
        while (index < line.size() &&
               std::isspace(static_cast<unsigned char>(line[index])) == 0) {
            started = true;
            if (line[index] == '\'') {
                ++index;
                while (index < line.size() && line[index] != '\'') {
                    token.push_back(line[index++]);
                }
                if (index == line.size()) {
                    return false;
                }
            } else if (line[index] == '"') {
                ++index;
                while (index < line.size() && line[index] != '"') {
                    if (line[index] == '\\' && !append_escape(line, index, token)) {
                        return false;
                    } else if (line[index] != '\\') {
                        token.push_back(line[index]);
                    }
                    ++index;
                }
                if (index == line.size()) {
                    return false;
                }
            } else if (line[index] == '\\') {
                if (!append_escape(line, index, token)) {
                    return false;
                }
            } else {
                token.push_back(line[index]);
            }
            ++index;
        }
        if (started) {
            tokens.push_back(std::move(token));
        }
    }
    return true;
}

std::string uppercase(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(
            std::toupper(static_cast<unsigned char>(character)));
    }
    return value;
}

} // namespace

ParseResult parse_command(std::string_view line) {
    std::vector<std::string> tokens;
    if (!tokenize(line, tokens)) {
        return {};
    }
    if (tokens.empty()) {
        ParseResult result;
        result.empty = true;
        return result;
    }
    const auto name = uppercase(tokens.front());
    CommandKind kind{};
    std::size_t arity = 0;
    if (name == "PING") {
        kind = CommandKind::ping;
    } else if (name == "PUBLISH") {
        kind = CommandKind::publish;
        arity = 2U;
    } else if (name == "SUBSCRIBE") {
        kind = CommandKind::subscribe;
        arity = 1U;
    } else if (name == "UNSUBSCRIBE") {
        kind = CommandKind::unsubscribe;
        arity = 1U;
    } else if (name == "HELP") {
        kind = CommandKind::help;
    } else if (name == "EXIT" || name == "QUIT") {
        kind = CommandKind::exit;
    } else {
        return {};
    }
    if (tokens.size() != arity + 1U) {
        return {};
    }
    tokens.erase(tokens.begin());
    ParseResult result;
    result.valid = true;
    result.command = Command{kind, std::move(tokens)};
    return result;
}

std::string escape_bytes(std::string_view bytes, bool quote) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    if (quote) {
        result.push_back('"');
    }
    for (const unsigned char byte : bytes) {
        switch (byte) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        case '\0': result += "\\0"; break;
        default:
            if (byte >= 0x20U && byte <= 0x7EU) {
                result.push_back(static_cast<char>(byte));
            } else {
                result += "\\x";
                result.push_back(hex[byte >> 4U]);
                result.push_back(hex[byte & 0x0FU]);
            }
        }
    }
    if (quote) {
        result.push_back('"');
    }
    return result;
}

} // namespace mps::cli
