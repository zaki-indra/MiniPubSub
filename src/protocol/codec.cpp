#include "protocol/codec.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mps::protocol
{
namespace
{
std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8U) |
                                      static_cast<std::uint16_t>(p[1]));
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
           static_cast<std::uint32_t>(p[3]);
}

void write_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 8U);
    out[1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24U);
    out[1] = static_cast<std::uint8_t>(value >> 16U);
    out[2] = static_cast<std::uint8_t>(value >> 8U);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t checked_size(std::size_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("protocol field exceeds uint32_t");
    }
    return static_cast<std::uint32_t>(size);
}
} // namespace

FrameDecoder::FrameDecoder(std::uint32_t max_payload)
    : max_payload_(max_payload) {
    pending_.reserve(header_size);
}

DecodeBatch FrameDecoder::consume(std::span<const std::uint8_t> input) {
    DecodeBatch result;
    if (fatal_) {
        result.status = DecodeStatus::fatal_length;
        return result;
    }
    pending_.insert(pending_.end(), input.begin(), input.end());
    std::size_t offset = 0;
    while (pending_.size() - offset >= header_size) {
        const auto* header = pending_.data() + offset;
        if (read_u32(header) != magic) {
            result.status = DecodeStatus::fatal_magic;
            fatal_ = true;
            break;
        }
        if (header[4] != version) {
            result.status = DecodeStatus::fatal_version;
            fatal_ = true;
            break;
        }
        const auto payload_size = read_u32(header + 12);
        if (payload_size > max_payload_) {
            result.status = DecodeStatus::fatal_length;
            fatal_ = true;
            break;
        }
        const auto total = header_size + static_cast<std::size_t>(payload_size);
        if (pending_.size() - offset < total) {
            break;
        }
        Frame frame;
        frame.opcode = header[5];
        frame.flags = read_u16(header + 6);
        frame.request_id = read_u32(header + 8);
        frame.payload.assign(header + header_size, header + total);
        result.frames.push_back(std::move(frame));
        offset += total;
    }
    if (offset != 0U) {
        pending_.erase(pending_.begin(),
                       pending_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    if (fatal_) {
        pending_.clear();
    }
    return result;
}

void FrameDecoder::reset() noexcept {
    pending_.clear();
    fatal_ = false;
}

SharedFrame encode_frame(std::uint8_t opcode, std::uint32_t request_id,
                         std::span<const std::uint8_t> payload) {
    auto out = std::make_shared<std::vector<std::uint8_t>>();
    out->resize(header_size + payload.size());
    write_u32(out->data(), magic);
    (*out)[4] = version;
    (*out)[5] = opcode;
    write_u16(out->data() + 6U, 0U);
    write_u32(out->data() + 8U, request_id);
    write_u32(out->data() + 12U, checked_size(payload.size()));
    std::copy(payload.begin(), payload.end(), out->begin() +
                                              static_cast<std::ptrdiff_t>(
                                                  header_size));
    return out;
}

std::vector<std::uint8_t> encode_channel(std::string_view channel) {
    std::vector<std::uint8_t> out;
    const auto total = 4U + channel.size();
    (void)checked_size(total);
    out.resize(total);
    write_u32(out.data(), checked_size(channel.size()));
    if (!channel.empty()) {
        std::memcpy(out.data() + 4U, channel.data(), channel.size());
    }
    return out;
}

std::vector<std::uint8_t> encode_channel_message(
    std::string_view channel, std::span<const std::uint8_t> message) {
    std::vector<std::uint8_t> out;
    const auto message_offset = 4U + channel.size();
    const auto total = message_offset + 4U + message.size();
    (void)checked_size(total);
    out.resize(total);
    write_u32(out.data(), checked_size(channel.size()));
    if (!channel.empty()) {
        std::memcpy(out.data() + 4U, channel.data(), channel.size());
    }
    write_u32(out.data() + message_offset, checked_size(message.size()));
    std::copy(message.begin(), message.end(),
              out.begin() + static_cast<std::ptrdiff_t>(message_offset + 4U));
    return out;
}

std::vector<std::uint8_t> encode_error(ErrorCode code,
                                       std::string_view message) {
    std::vector<std::uint8_t> out;
    const auto total = 6U + message.size();
    (void)checked_size(total);
    out.resize(total);
    write_u16(out.data(), static_cast<std::uint16_t>(code));
    write_u32(out.data() + 2U, checked_size(message.size()));
    if (!message.empty()) {
        std::memcpy(out.data() + 6U, message.data(), message.size());
    }
    return out;
}

bool decode_channel(std::span<const std::uint8_t> payload,
                    std::uint32_t max_channel, std::string& channel,
                    ErrorCode& error) {
    if (payload.size() < 4U) {
        error = ErrorCode::malformed_frame;
        return false;
    }
    const auto length = read_u32(payload.data());
    if (length == 0U) {
        error = ErrorCode::invalid_channel;
        return false;
    }
    if (length > max_channel) {
        error = ErrorCode::channel_too_long;
        return false;
    }
    if (payload.size() != 4U + static_cast<std::size_t>(length)) {
        error = ErrorCode::malformed_frame;
        return false;
    }
    channel.assign(reinterpret_cast<const char*>(payload.data() + 4U), length);
    return true;
}

bool decode_channel_message(std::span<const std::uint8_t> payload,
                            std::uint32_t max_channel,
                            std::uint32_t max_message, ChannelMessage& value,
                            ErrorCode& error) {
    if (payload.size() < 8U) {
        error = ErrorCode::malformed_frame;
        return false;
    }
    const auto channel_length = read_u32(payload.data());
    if (channel_length == 0U) {
        error = ErrorCode::invalid_channel;
        return false;
    }
    if (channel_length > max_channel) {
        error = ErrorCode::channel_too_long;
        return false;
    }
    const auto message_offset = 4U + static_cast<std::size_t>(channel_length);
    if (message_offset + 4U > payload.size()) {
        error = ErrorCode::malformed_frame;
        return false;
    }
    const auto message_length = read_u32(payload.data() + message_offset);
    if (message_length > max_message) {
        error = ErrorCode::message_too_large;
        return false;
    }
    if (payload.size() != message_offset + 4U +
        static_cast<std::size_t>(message_length)) {
        error = ErrorCode::malformed_frame;
        return false;
    }
    value.channel.assign(reinterpret_cast<const char*>(payload.data() + 4U),
                         channel_length);
    value.message.assign(payload.begin() +
                         static_cast<std::ptrdiff_t>(message_offset + 4U),
                         payload.end());
    return true;
}

bool decode_error(std::span<const std::uint8_t> payload,
                  std::uint32_t max_error_message, RemoteError& value) {
    if (payload.size() < 6U) {
        return false;
    }
    const auto length = read_u32(payload.data() + 2U);
    if (length > max_error_message ||
        payload.size() != 6U + static_cast<std::size_t>(length)) {
        return false;
    }
    value.code = static_cast<ErrorCode>(read_u16(payload.data()));
    value.message.assign(reinterpret_cast<const char*>(payload.data() + 6U),
                         length);
    return true;
}
} // namespace mps::protocol