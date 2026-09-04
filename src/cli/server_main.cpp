#include "minipubsub/minipubsub.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>

namespace
{
std::atomic<bool> stopping{false};

void stop_signal(int) {
    stopping.store(true);
}

void log_record(const mps_log_record_t* record, void*) {
    std::cerr << '[' << record->component << "] " << record->event_name << ": "
        << record->message << '\n';
}
} // namespace

int main(int argc, char** argv) {
    const char* bind = "127.0.0.1";
    unsigned long port = 7379;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--bind" && index + 1 < argc) {
            bind = argv[++index];
        } else if (argument == "--port" && index + 1 < argc) {
            port = std::strtoul(argv[++index], nullptr, 10);
        } else {
            std::cerr << "usage: minipubsub-server [--bind ADDRESS] [--port PORT]\n";
            return 2;
        }
    }
    if (port > 65535UL) {
        std::cerr << "invalid port\n";
        return 2;
    }

    mps_diagnostic_t diagnostic{};
    mps_server_config_t* config = nullptr;
    mps_server_t* server = nullptr;
    if (mps_server_config_create(&config, &diagnostic) != MPS_STATUS_OK ||
        mps_server_config_set_bind_address(config, bind, &diagnostic) !=
        MPS_STATUS_OK ||
        mps_server_config_set_port(config, static_cast<std::uint16_t>(port),
                                   &diagnostic) != MPS_STATUS_OK ||
        mps_server_config_set_log_callback(config, log_record, nullptr,
                                           &diagnostic) != MPS_STATUS_OK ||
        mps_server_create(config, &server, &diagnostic) != MPS_STATUS_OK) {
        std::cerr << diagnostic.message << '\n';
        mps_server_config_destroy(config);
        return 1;
    }
    mps_server_config_destroy(config);
    if (mps_server_start(server, &diagnostic) != MPS_STATUS_OK) {
        std::cerr << diagnostic.message << '\n';
        mps_server_destroy(server);
        return 1;
    }
    std::uint16_t bound_port = 0;
    (void)mps_server_bound_port(server, &bound_port, nullptr);
    std::cout << "listening on " << bind << ':' << bound_port << '\n';
    std::signal(SIGINT, stop_signal);
    std::signal(SIGTERM, stop_signal);
    while (!stopping.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    (void)mps_server_request_stop(server, nullptr);
    (void)mps_server_wait(server, nullptr);
    mps_server_destroy(server);
    return 0;
}