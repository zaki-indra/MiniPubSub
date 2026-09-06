#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mps::protocol
{
inline constexpr std::uint32_t magic = 0x4D505300U;
inline constexpr std::uint8_t version = 1U;
inline constexpr std::size_t header_size = 16U;

enum class Opcode : std::uint8_t {
    publish = 0x01,
    subscribe = 0x02,
    unsubscribe = 0x03,
    ping = 0x04,
    ok = 0x80,
    message = 0x81,
    error = 0x82,
    pong = 0x83,
};

enum class ErrorCode : std::uint16_t {
    unknown_opcode = 0x0001,
    malformed_frame = 0x0002,
    channel_too_long = 0x0003,
    message_too_large = 0x0004,
    invalid_channel = 0x0005,
    invalid_request = 0x0006,
    internal_error = 0x0007,
};

struct Frame {
    std::uint8_t opcode{};
    std::uint16_t flags{};
    std::uint32_t request_id{};
    std::vector<std::uint8_t> payload;
};

using SharedFrame = std::shared_ptr<const std::vector<std::uint8_t>>;

struct ChannelMessage {
    std::string channel;
    std::vector<std::uint8_t> message;
};

struct RemoteError {
    ErrorCode code{};
    std::string message;
};
} // namespace mps::protocol