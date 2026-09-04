#include "minipubsub/minipubsub.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace
{
mps_bytes_view_t view(std::string_view value) {
    return {reinterpret_cast<const std::uint8_t*>(value.data()),
            static_cast<std::uint32_t>(value.size())};
}

int fail(const mps_diagnostic_t& diagnostic) {
    std::cerr << diagnostic.message << '\n';
    return 1;
}
} // namespace

int main(int argc, char** argv) {
    const char* host = "127.0.0.1";
    unsigned long port = 7379;
    int index = 1;
    while (index < argc && std::string_view(argv[index]).starts_with("--")) {
        const std::string_view option = argv[index++];
        if (index >= argc) {
            std::cerr << "missing option value\n";
            return 2;
        }
        if (option == "--host") {
            host = argv[index++];
        } else if (option == "--port") {
            port = std::strtoul(argv[index++], nullptr, 10);
        } else {
            std::cerr << "unknown option\n";
            return 2;
        }
    }
    if (port == 0UL || port > 65535UL || index >= argc) {
        std::cerr << "usage: minipubsub-cli [--host HOST] [--port PORT] "
            "ping|publish CHANNEL MESSAGE|subscribe CHANNEL|unsubscribe "
            "CHANNEL\n";
        return 2;
    }
    const std::string command = argv[index++];
    mps_diagnostic_t diagnostic{};
    mps_client_config_t* config = nullptr;
    mps_client_t* client = nullptr;
    if (mps_client_config_create(&config, &diagnostic) != MPS_STATUS_OK ||
        mps_client_config_set_host(config, host, &diagnostic) != MPS_STATUS_OK ||
        mps_client_config_set_port(config, static_cast<std::uint16_t>(port),
                                   &diagnostic) != MPS_STATUS_OK ||
        mps_client_create(config, &client, &diagnostic) != MPS_STATUS_OK) {
        mps_client_config_destroy(config);
        return fail(diagnostic);
    }
    mps_client_config_destroy(config);
    if (mps_client_connect(client, &diagnostic) != MPS_STATUS_OK) {
        mps_client_destroy(client);
        return fail(diagnostic);
    }

    mps_request_id_t request = 0;
    mps_status_t status = MPS_STATUS_INVALID_ARGUMENT;
    if (command == "ping" && index == argc) {
        status = mps_client_ping(client, &request, &diagnostic);
    } else if (command == "publish" && index + 2 == argc) {
        const auto channel = view(argv[index]);
        const auto message = view(argv[index + 1]);
        status = mps_client_publish(client, &channel, &message, &request,
                                    &diagnostic);
    } else if (command == "subscribe" && index + 1 == argc) {
        const auto channel = view(argv[index]);
        status = mps_client_subscribe(client, &channel, &request,
                                      &diagnostic);
    } else if (command == "unsubscribe" && index + 1 == argc) {
        const auto channel = view(argv[index]);
        status = mps_client_unsubscribe(client, &channel, &request,
                                        &diagnostic);
    } else {
        std::cerr << "invalid command arguments\n";
        mps_client_destroy(client);
        return 2;
    }
    if (status != MPS_STATUS_OK) {
        mps_client_destroy(client);
        return fail(diagnostic);
    }

    for (;;) {
        mps_event_t* event = nullptr;
        status = mps_client_poll_event(client, 30000U, &event, &diagnostic);
        if (status != MPS_STATUS_OK) {
            mps_client_destroy(client);
            return fail(diagnostic);
        }
        const auto kind = mps_event_kind(event);
        if (kind == MPS_EVENT_MESSAGE) {
            mps_bytes_view_t channel{};
            mps_bytes_view_t message{};
            (void)mps_event_channel(event, &channel);
            (void)mps_event_message(event, &message);
            std::cout.write(reinterpret_cast<const char*>(channel.data),
                            channel.length);
            std::cout << ' ';
            std::cout.write(reinterpret_cast<const char*>(message.data),
                            message.length);
            std::cout << '\n';
            mps_event_destroy(event);
            if (command == "subscribe") {
                continue;
            }
        } else if (kind == MPS_EVENT_REMOTE_ERROR) {
            std::cerr << mps_event_error_message(event) << '\n';
            mps_event_destroy(event);
            mps_client_destroy(client);
            return 1;
        } else if (mps_event_request_id(event) == request) {
            std::cout << (kind == MPS_EVENT_PONG ? "PONG" : "OK") << '\n';
            mps_event_destroy(event);
            if (command != "subscribe") {
                break;
            }
            continue;
        }
        mps_event_destroy(event);
    }
    mps_client_destroy(client);
    return 0;
}