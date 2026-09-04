#pragma once

#include "protocol/codec.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mps::core
{

using ConnectionId = std::uint64_t;

enum class CloseReason { protocol_error, queue_limit, io_error, shutdown };

class DeliverySink {
public:
    virtual ~DeliverySink() = default;
    virtual bool enqueue(ConnectionId id, protocol::SharedFrame frame) = 0;
    virtual void close(ConnectionId id, CloseReason reason) = 0;
};

class Broker {
public:
    Broker(DeliverySink& sink, std::uint32_t max_channel,
           std::uint32_t max_message);
    void connection_opened(ConnectionId id);
    void execute(ConnectionId id, const protocol::Frame& request);
    void connection_closed(ConnectionId id);

private:
    void send_ok(ConnectionId id, std::uint32_t request_id);
    void send_error(ConnectionId id, std::uint32_t request_id,
                    protocol::ErrorCode code, std::string_view message);

    DeliverySink& sink_;
    std::uint32_t max_channel_;
    std::uint32_t max_message_;
    std::unordered_map<std::string, std::unordered_set<ConnectionId>> channels_;
    std::unordered_map<ConnectionId, std::unordered_set<std::string>> reverse_;
};

} // namespace mps::core