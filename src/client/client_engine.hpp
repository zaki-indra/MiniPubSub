#pragma once

#include "minipubsub/types.h"
#include "protocol/codec.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mps::client
{

struct Event {
    mps_event_kind_t kind{0};
    std::uint32_t request_id{0};
    std::string channel;
    std::vector<std::uint8_t> message;
    mps_protocol_error_t protocol_error{0};
    std::string error_message;
    mps_disconnect_cause_t disconnect_cause{0};
};

struct Config {
    std::string host{"127.0.0.1"};
    std::uint16_t port{7379};
    std::uint32_t connect_timeout_ms{5000};
    std::uint32_t shutdown_timeout_ms{5000};
    std::uint32_t command_queue_limit{1024};
    std::uint32_t command_queue_bytes_limit{4194304};
    std::uint32_t event_queue_limit{1024};
    std::uint32_t event_queue_bytes_limit{4194304};
    mps_event_delivery_mode_t event_mode{MPS_EVENT_MODE_POLL};
    std::function<void(const Event&)> callback;
};

class ClientEngine {
public:
    explicit ClientEngine(Config config);
    ~ClientEngine();
    ClientEngine(const ClientEngine&) = delete;
    ClientEngine& operator=(const ClientEngine&) = delete;

    mps_status_t connect(std::string& error, int& system_code);
    mps_status_t publish(std::string_view channel,
                         std::span<const std::uint8_t> message,
                         std::uint32_t& request_id);
    mps_status_t subscribe(std::string_view channel, std::uint32_t& request_id);
    mps_status_t unsubscribe(std::string_view channel,
                             std::uint32_t& request_id);
    mps_status_t ping(std::uint32_t& request_id);
    mps_status_t poll(std::uint32_t timeout_ms, std::unique_ptr<Event>& event);
    void request_stop() noexcept;
    mps_status_t wait() noexcept;
    [[nodiscard]] mps_event_delivery_mode_t event_mode() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mps::client

struct mps_event {
    mps::client::Event value;
    bool borrowed{false};
};