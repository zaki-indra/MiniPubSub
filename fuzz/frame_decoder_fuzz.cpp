#include "protocol/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                       std::size_t size) {
  mps::protocol::FrameDecoder decoder(1049608U);
  (void)decoder.consume(std::span(data, size));
  return 0;
}
