#pragma once

#include "protocol/protocol_types.hpp"

#include <span>
#include <string>
#include <vector>

namespace mps::protocol
{
enum class DecodeStatus { ok, fatal_magic, fatal_version, fatal_length };

struct DecodeBatch {
    std::vector<Frame> frames;
    DecodeStatus status{DecodeStatus::ok};
};

class FrameDecoder {
public:
    explicit FrameDecoder(std::uint32_t max_payload);
    DecodeBatch consume(std::span<const std::uint8_t> input);
    void reset() noexcept;

private:
    std::uint32_t max_payload_;
    std::vector<std::uint8_t> pending_;
    bool fatal_{false};
};

SharedFrame encode_frame(std::uint8_t opcode, std::uint32_t request_id,
                         std::span<const std::uint8_t> payload = {});
std::vector<std::uint8_t> encode_channel(std::string_view channel);
std::vector<std::uint8_t> encode_channel_message(
    std::string_view channel, std::span<const std::uint8_t> message);
std::vector<std::uint8_t> encode_error(ErrorCode code,
                                       std::string_view message);

bool decode_channel(std::span<const std::uint8_t> payload,
                    std::uint32_t max_channel, std::string& channel,
                    ErrorCode& error);
bool decode_channel_message(std::span<const std::uint8_t> payload,
                            std::uint32_t max_channel,
                            std::uint32_t max_message, ChannelMessage& value,
                            ErrorCode& error);
bool decode_error(std::span<const std::uint8_t> payload,
                  std::uint32_t max_error_message, RemoteError& value);
} // namespace mps::protocol