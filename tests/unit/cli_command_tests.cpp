#include "cli/client_command.hpp"

#include <cassert>
#include <string>

using mps::cli::CommandKind;

int main() {
    {
        const auto result = mps::cli::parse_command("  pInG  ");
        assert(result.valid);
        assert(result.command.kind == CommandKind::ping);
    }
    {
        const auto result = mps::cli::parse_command(
            "PUBLISH 'channel one' pre\"hello world\"post");
        assert(result.valid);
        assert(result.command.kind == CommandKind::publish);
        assert(result.command.arguments[0] == "channel one");
        assert(result.command.arguments[1] == "prehello worldpost");
    }
    {
        const auto result = mps::cli::parse_command("PUBLISH c \"\"");
        assert(result.valid);
        assert(result.command.arguments[1].empty());
    }
    {
        const auto result = mps::cli::parse_command(
            "PUBLISH c \\x41\\n\\r\\t\\0\\\\\\\"");
        assert(result.valid);
        const std::string expected{"A\n\r\t\0\\\"", 7U};
        assert(result.command.arguments[1] == expected);
    }
    {
        const auto result = mps::cli::parse_command("SuBsCrIbE topic");
        assert(result.valid);
        assert(result.command.kind == CommandKind::subscribe);
    }
    {
        const auto result = mps::cli::parse_command("unSUBSCRIBE topic");
        assert(result.valid);
        assert(result.command.kind == CommandKind::unsubscribe);
    }
    assert(mps::cli::parse_command("   \t").empty);
    assert(mps::cli::parse_command("PUBLISH c").valid == false);
    assert(mps::cli::parse_command("PING extra").valid == false);
    assert(mps::cli::parse_command("PUBLISH c 'unterminated").valid == false);
    assert(mps::cli::parse_command("PUBLISH c trailing\\").valid == false);
    assert(mps::cli::parse_command("PUBLISH c \\x0Z").valid == false);
    assert(mps::cli::parse_command("unknown").valid == false);
    assert(mps::cli::parse_command("help").command.kind == CommandKind::help);
    assert(mps::cli::parse_command("quit").command.kind == CommandKind::exit);

    const std::string bytes{"a\n\0\x01\"\\", 6U};
    assert(mps::cli::escape_bytes(bytes, true) == "\"a\\n\\0\\x01\\\"\\\\\"");
    return 0;
}
