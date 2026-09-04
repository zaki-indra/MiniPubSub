#include "core/broker.hpp"

#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
class Sink final : public mps::core::DeliverySink {
 public:
  bool enqueue(mps::core::ConnectionId id,
               mps::protocol::SharedFrame frame) override {
    frames[id].push_back(std::move(frame));
    return accepted;
  }
  void close(mps::core::ConnectionId id, mps::core::CloseReason) override {
    closed.push_back(id);
  }

  bool accepted{true};
  std::unordered_map<mps::core::ConnectionId,
                     std::vector<mps::protocol::SharedFrame>>
      frames;
  std::vector<mps::core::ConnectionId> closed;
};

mps::protocol::Frame request(mps::protocol::Opcode opcode, std::uint32_t id,
                             std::vector<std::uint8_t> payload = {}) {
  return {static_cast<std::uint8_t>(opcode), 0U, id, std::move(payload)};
}
}  // namespace

int main() {
  Sink sink;
  mps::core::Broker broker(sink, 32U, 64U);
  broker.connection_opened(1U);
  broker.connection_opened(2U);
  broker.connection_opened(3U);

  const auto channel = mps::protocol::encode_channel("room");
  broker.execute(1U, request(mps::protocol::Opcode::subscribe, 1U, channel));
  broker.execute(1U, request(mps::protocol::Opcode::subscribe, 2U, channel));
  broker.execute(2U, request(mps::protocol::Opcode::subscribe, 3U, channel));
  assert(sink.frames[1U].size() == 2U);
  assert(sink.frames[2U].size() == 1U);

  const std::vector<std::uint8_t> bytes{0U, 42U};
  const auto publication =
      mps::protocol::encode_channel_message("room", bytes);
  broker.execute(3U,
                 request(mps::protocol::Opcode::publish, 4U, publication));
  assert(sink.frames[1U].size() == 3U);
  assert(sink.frames[2U].size() == 2U);
  assert(sink.frames[3U].size() == 1U);
  assert(sink.frames[1U].back() == sink.frames[2U].back());

  broker.execute(1U,
                 request(mps::protocol::Opcode::unsubscribe, 5U, channel));
  broker.execute(1U,
                 request(mps::protocol::Opcode::unsubscribe, 6U, channel));
  broker.connection_closed(2U);
  broker.execute(3U,
                 request(mps::protocol::Opcode::publish, 7U, publication));
  assert(sink.frames[1U].size() == 5U);
  assert(sink.frames[2U].size() == 2U);
  assert(sink.frames[3U].size() == 2U);

  broker.execute(3U, request(mps::protocol::Opcode::subscribe, 0U, channel));
  assert(sink.frames[3U].size() == 3U);
  return 0;
}
