#pragma once

#include "core/broker.hpp"
#include "minipubsub/types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace mps::server
{

struct Config {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{7379};
    std::uint32_t max_connections{128};
    std::uint32_t max_channel_size{1024};
    std::uint32_t max_message_size{1048576};
    std::uint32_t output_queue_limit{4194304};
    std::uint32_t command_queue_limit{4096};
    std::uint32_t command_queue_bytes_limit{67108864};
    std::uint32_t shutdown_timeout_ms{5000};
    mps_log_callback_t log_callback{nullptr};
    void* log_user_data{nullptr};
};

class ServerRuntime {
public:
    explicit ServerRuntime(Config config);
    ~ServerRuntime();
    ServerRuntime(const ServerRuntime&) = delete;
    ServerRuntime& operator=(const ServerRuntime&) = delete;

    mps_status_t start(std::string& error, int& system_code);
    std::uint16_t bound_port() const noexcept;
    void request_stop() noexcept;
    void wait() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mps::server