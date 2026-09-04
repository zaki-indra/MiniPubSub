#include "protocol/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                       std::size_t size) {
  mps::protocol::ChannelMessage message;
  mps::protocol::ErrorCode error{};
  (void)mps::protocol::decode_channel_message(std::span(data, size), 1024U,
                                              1048576U, message, error);
  return 0;
}
