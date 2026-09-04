#include "protocol/codec.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

int main() {
  using namespace mps::protocol;
  const std::array<std::uint8_t, 3> message{0U, 1U, 255U};
  const auto payload = encode_channel_message("a", message);
  const auto encoded = encode_frame(static_cast<std::uint8_t>(Opcode::publish),
                                    0x01020304U, payload);
  const std::array<std::uint8_t, 16> expected_header{
      0x4d, 0x50, 0x53, 0x00, 0x01, 0x01, 0x00, 0x00,
      0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x0c};
  assert(std::equal(expected_header.begin(), expected_header.end(),
                    encoded->begin()));

  FrameDecoder decoder(1024U);
  DecodeBatch batch;
  for (const auto byte : *encoded) {
    auto next = decoder.consume(std::span(&byte, 1U));
    assert(next.status == DecodeStatus::ok);
    if (!next.frames.empty()) {
      batch = std::move(next);
    }
  }
  assert(batch.frames.size() == 1U);
  assert(batch.frames[0].request_id == 0x01020304U);
  ChannelMessage decoded;
  ErrorCode error{};
  assert(decode_channel_message(batch.frames[0].payload, 8U, 8U, decoded,
                                error));
  assert(decoded.channel == "a");
  assert(decoded.message == std::vector<std::uint8_t>(message.begin(),
                                                       message.end()));

  FrameDecoder multi(1024U);
  std::vector<std::uint8_t> joined = *encoded;
  joined.insert(joined.end(), encoded->begin(), encoded->end());
  const auto two = multi.consume(joined);
  assert(two.frames.size() == 2U);

  auto bad_magic = *encoded;
  bad_magic[0] = 0U;
  FrameDecoder invalid(1024U);
  assert(invalid.consume(bad_magic).status == DecodeStatus::fatal_magic);

  auto bad_version = *encoded;
  bad_version[4] = 2U;
  FrameDecoder wrong_version(1024U);
  assert(wrong_version.consume(bad_version).status ==
         DecodeStatus::fatal_version);

  FrameDecoder too_large(4U);
  assert(too_large.consume(*encoded).status == DecodeStatus::fatal_length);

  auto trailing = encode_channel("x");
  trailing.push_back(0U);
  std::string channel;
  assert(!decode_channel(trailing, 8U, channel, error));
  assert(error == ErrorCode::malformed_frame);
  return 0;
}
