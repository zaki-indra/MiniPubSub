#include "minipubsub/minipubsub.h"

#include "client/client_engine.hpp"
#include "server/server_runtime.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

struct mps_client_config {
    mps::client::Config value;
    mps_event_callback_t callback{nullptr};
    void* callback_user_data{nullptr};
    bool consumed{false};
};

struct mps_server_config {
    mps::server::Config value;
    bool consumed{false};
};

struct mps_client {
    explicit mps_client(mps::client::Config config)
        : engine(std::move(config)) {
    }

    mps::client::ClientEngine engine;
};

struct mps_server {
    explicit mps_server(mps::server::Config config)
        : runtime(std::move(config)) {
    }

    mps::server::ServerRuntime runtime;
    bool started{false};
};

namespace
{

void diagnostic(mps_diagnostic_t* out, int system_code,
                std::string_view message) noexcept {
    if (out == nullptr) {
        return;
    }
    out->struct_size = sizeof(*out);
    out->system_code = system_code;
    const auto size = std::min(message.size(), sizeof(out->message) - 1U);
    if (size != 0U) {
        std::copy_n(message.data(), size, out->message);
    }
    out->message[size] = '\0';
}

void clear_diagnostic(mps_diagnostic_t* out) noexcept {
    diagnostic(out, 0, {});
}

template <typename Function>
mps_status_t boundary(mps_diagnostic_t* out, Function&& function) noexcept {
    clear_diagnostic(out);
    try {
        return function();
    } catch (const std::bad_alloc&) {
        diagnostic(out, 0, "out of memory");
        return MPS_STATUS_OUT_OF_MEMORY;
    } catch (const std::exception& exception) {
        diagnostic(out, 0, exception.what());
        return MPS_STATUS_INTERNAL_ERROR;
    } catch (...) {
        diagnostic(out, 0, "unknown internal failure");
        return MPS_STATUS_INTERNAL_ERROR;
    }
}

template <typename Config>
mps_status_t mutable_config(Config* config, mps_diagnostic_t* out) {
    if (config == nullptr) {
        diagnostic(out, 0, "configuration is null");
        return MPS_STATUS_INVALID_ARGUMENT;
    }
    if (config->consumed) {
        diagnostic(out, 0, "configuration has already been consumed");
        return MPS_STATUS_INVALID_STATE;
    }
    return MPS_STATUS_OK;
}

bool valid_text(const char* text) {
    return text != nullptr && text[0] != '\0';
}

bool valid_view(mps_bytes_view_t view) {
    return view.length == 0U || view.data != nullptr;
}

template <typename Submit>
mps_status_t client_submit(mps_client_t* client,
                           mps_request_id_t* out_request_id,
                           mps_diagnostic_t* out, Submit&& submit) {
    return boundary(out, [&] {
        if (client == nullptr || out_request_id == nullptr) {
            diagnostic(out, 0, "client or output request ID is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        const auto status = submit(client->engine, *out_request_id);
        if (status != MPS_STATUS_OK) {
            diagnostic(out, 0, "command was not accepted");
        }
        return status;
    });
}

std::string_view as_string_view(mps_bytes_view_t view) {
    if (view.length == 0U) {
        return {};
    }
    return {reinterpret_cast<const char*>(view.data), view.length};
}

std::span<const std::uint8_t> as_span(mps_bytes_view_t view) {
    return {view.data, view.length};
}

} // namespace

extern "C" {

uint32_t MPS_CALL mps_abi_version(void) {
    return MPS_ABI_VERSION;
}

uint32_t MPS_CALL mps_protocol_version(void) {
    return MPS_PROTOCOL_VERSION;
}

const char* MPS_CALL mps_version_string(void) {
    return "1.0.0";
}

mps_status_t MPS_CALL
mps_client_config_create(mps_client_config_t** out_config,
                         mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (out_config == nullptr) {
            diagnostic(out, 0, "output configuration pointer is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        *out_config = nullptr;
        *out_config = new mps_client_config{};
        return MPS_STATUS_OK;
    });
}

mps_status_t MPS_CALL
mps_client_config_set_host(mps_client_config_t* config, const char* host,
                           mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (const auto status = mutable_config(config, out);
            status != MPS_STATUS_OK) {
            return status;
        }
        if (!valid_text(host)) {
            diagnostic(out, 0, "host must be non-empty");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        config->value.host = host;
        return MPS_STATUS_OK;
    });
}

mps_status_t MPS_CALL
mps_client_config_set_port(mps_client_config_t* config, uint16_t port,
                           mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (const auto status = mutable_config(config, out);
            status != MPS_STATUS_OK) {
            return status;
        }
        if (port == 0U) {
            diagnostic(out, 0, "client port must be non-zero");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        config->value.port = port;
        return MPS_STATUS_OK;
    });
}

#define MPS_CLIENT_POSITIVE_SETTER(name, field)                                  \
  mps_status_t MPS_CALL name(mps_client_config_t *config, uint32_t value,        \
                             mps_diagnostic_t *out) {                            \
    return boundary(out, [&] {                                                   \
      if (const auto status = mutable_config(config, out);                       \
          status != MPS_STATUS_OK) {                                             \
        return status;                                                           \
      }                                                                          \
      if (value == 0U || value > static_cast<uint32_t>(INT32_MAX)) {             \
        diagnostic(out, 0, "value must be between 1 and INT32_MAX");            \
        return MPS_STATUS_INVALID_ARGUMENT;                                      \
      }                                                                          \
      config->value.field = value;                                               \
      return MPS_STATUS_OK;                                                      \
    });                                                                          \
  }

MPS_CLIENT_POSITIVE_SETTER(mps_client_config_set_connect_timeout_ms,
                           connect_timeout_ms)
MPS_CLIENT_POSITIVE_SETTER(mps_client_config_set_shutdown_timeout_ms,
                           shutdown_timeout_ms)
MPS_CLIENT_POSITIVE_SETTER(mps_client_config_set_command_queue_limit,
                           command_queue_limit)
MPS_CLIENT_POSITIVE_SETTER(mps_client_config_set_event_queue_limit,
                           event_queue_limit)

#undef MPS_CLIENT_POSITIVE_SETTER

mps_status_t MPS_CALL mps_client_config_set_event_callback(
    mps_client_config_t* config, mps_event_callback_t callback, void* user_data,
    mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (const auto status = mutable_config(config, out);
            status != MPS_STATUS_OK) {
            return status;
        }
        if (callback == nullptr) {
            diagnostic(out, 0, "callback is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        config->callback = callback;
        config->callback_user_data = user_data;
        config->value.event_mode = MPS_EVENT_MODE_CALLBACK;
        return MPS_STATUS_OK;
    });
}

void MPS_CALL mps_client_config_destroy(mps_client_config_t* config) {
    try {
        delete config;
    } catch (...) {
    }
}

mps_status_t MPS_CALL
mps_client_create(const mps_client_config_t* config, mps_client_t** out_client,
                  mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (config == nullptr || out_client == nullptr) {
            diagnostic(out, 0, "configuration or output client pointer is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        *out_client = nullptr;
        auto snapshot = config->value;
        if (snapshot.event_mode == MPS_EVENT_MODE_CALLBACK) {
            const auto callback = config->callback;
            void* const user_data = config->callback_user_data;
            snapshot.callback = [callback, user_data](const mps::client::Event& event) {
                mps_event wrapper{event, true};
                callback(&wrapper, user_data);
            };
        }
        *out_client = new mps_client(std::move(snapshot));
        const_cast<mps_client_config_t*>(config)->consumed = true;
        return MPS_STATUS_OK;
    });
}

mps_status_t MPS_CALL mps_client_connect(mps_client_t* client,
                                         mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (client == nullptr) {
            diagnostic(out, 0, "client is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        std::string error;
        int system_code = 0;
        const auto status = client->engine.connect(error, system_code);
        if (status != MPS_STATUS_OK) {
            diagnostic(out, system_code, error);
        }
        return status;
    });
}

mps_status_t MPS_CALL mps_client_publish(
    mps_client_t* client, const mps_bytes_view_t* channel,
    const mps_bytes_view_t* message,
    mps_request_id_t* out_request_id, mps_diagnostic_t* out) {
    if (channel == nullptr || message == nullptr || !valid_view(*channel) ||
        !valid_view(*message)) {
        diagnostic(out, 0, "invalid byte view");
        return MPS_STATUS_INVALID_ARGUMENT;
    }
    return client_submit(client, out_request_id, out, [&](auto& engine, auto& id) {
        return engine.publish(as_string_view(*channel), as_span(*message), id);
    });
}

mps_status_t MPS_CALL mps_client_subscribe(
    mps_client_t* client, const mps_bytes_view_t* channel,
    mps_request_id_t* out_request_id, mps_diagnostic_t* out) {
    if (channel == nullptr || !valid_view(*channel)) {
        diagnostic(out, 0, "invalid byte view");
        return MPS_STATUS_INVALID_ARGUMENT;
    }
    return client_submit(client, out_request_id, out, [&](auto& engine, auto& id) {
        return engine.subscribe(as_string_view(*channel), id);
    });
}

mps_status_t MPS_CALL mps_client_unsubscribe(
    mps_client_t* client, const mps_bytes_view_t* channel,
    mps_request_id_t* out_request_id, mps_diagnostic_t* out) {
    if (channel == nullptr || !valid_view(*channel)) {
        diagnostic(out, 0, "invalid byte view");
        return MPS_STATUS_INVALID_ARGUMENT;
    }
    return client_submit(client, out_request_id, out, [&](auto& engine, auto& id) {
        return engine.unsubscribe(as_string_view(*channel), id);
    });
}

mps_status_t MPS_CALL mps_client_ping(mps_client_t* client,
                                      mps_request_id_t* out_request_id,
                                      mps_diagnostic_t* out) {
    return client_submit(client, out_request_id, out,
                         [](auto& engine, auto& id) {
                             return engine.ping(id);
                         });
}

mps_status_t MPS_CALL
mps_client_poll_event(mps_client_t* client, uint32_t timeout_ms,
                      mps_event_t** out_event, mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (client == nullptr || out_event == nullptr) {
            diagnostic(out, 0, "client or output event pointer is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        *out_event = nullptr;
        std::unique_ptr<mps::client::Event> event;
        const auto status = client->engine.poll(timeout_ms, event);
        if (status != MPS_STATUS_OK) {
            if (status != MPS_STATUS_TIMEOUT) {
                diagnostic(out, 0, "event is not available");
            }
            return status;
        }
        *out_event = new mps_event{std::move(*event), false};
        return MPS_STATUS_OK;
    });
}

mps_status_t MPS_CALL mps_client_request_stop(mps_client_t* client,
                                              mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (client == nullptr) {
            diagnostic(out, 0, "client is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        client->engine.request_stop();
        return MPS_STATUS_OK;
    });
}

mps_status_t MPS_CALL mps_client_wait(mps_client_t* client,
                                      mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (client == nullptr) {
            diagnostic(out, 0, "client is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        const auto status = client->engine.wait();
        if (status != MPS_STATUS_OK) {
            diagnostic(out, 0, "wait cannot run on a client-owned thread");
        }
        return status;
    });
}

void MPS_CALL mps_client_destroy(mps_client_t* client) {
    if (client == nullptr) {
        return;
    }
    try {
        client->engine.request_stop();
        if (client->engine.wait() == MPS_STATUS_OK) {
            delete client;
        }
    } catch (...) {
    }
}

mps_event_kind_t MPS_CALL mps_event_kind(const mps_event_t* event) {
    return event == nullptr ? 0U : event->value.kind;
}

mps_request_id_t MPS_CALL mps_event_request_id(const mps_event_t* event) {
    return event == nullptr ? 0U : event->value.request_id;
}

mps_status_t MPS_CALL mps_event_channel(const mps_event_t* event,
                                        mps_bytes_view_t* out_channel) {
    if (event == nullptr || out_channel == nullptr) {
        return MPS_STATUS_INVALID_ARGUMENT;
    }
    out_channel->data = event->value.channel.empty()
                            ? nullptr
                            : reinterpret_cast<const std::uint8_t*>(
                                event->value.channel.data());
    out_channel->length =
        static_cast<std::uint32_t>(event->value.channel.size());
    return event->value.kind == MPS_EVENT_MESSAGE
               ? MPS_STATUS_OK
               : MPS_STATUS_INVALID_STATE;
}

mps_status_t MPS_CALL mps_event_message(const mps_event_t* event,
                                        mps_bytes_view_t* out_message) {
    if (event == nullptr || out_message == nullptr) {
        return MPS_STATUS_INVALID_ARGUMENT;
    }
    out_message->data =
        event->value.message.empty() ? nullptr : event->value.message.data();
    out_message->length =
        static_cast<std::uint32_t>(event->value.message.size());
    return event->value.kind == MPS_EVENT_MESSAGE
               ? MPS_STATUS_OK
               : MPS_STATUS_INVALID_STATE;
}

mps_protocol_error_t MPS_CALL
mps_event_protocol_error(const mps_event_t* event) {
    return event == nullptr ? 0U : event->value.protocol_error;
}

const char* MPS_CALL mps_event_error_message(const mps_event_t* event) {
    return event == nullptr ? "" : event->value.error_message.c_str();
}

mps_disconnect_cause_t MPS_CALL
mps_event_disconnect_cause(const mps_event_t* event) {
    return event == nullptr ? 0U : event->value.disconnect_cause;
}

void MPS_CALL mps_event_destroy(mps_event_t* event) {
    try {
        if (event != nullptr && !event->borrowed) {
            delete event;
        }
    } catch (...) {
    }
}

mps_status_t MPS_CALL
mps_server_config_create(mps_server_config_t** out_config,
                         mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (out_config == nullptr) {
            diagnostic(out, 0, "output configuration pointer is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        *out_config = nullptr;
        *out_config = new mps_server_config{};
        return MPS_STATUS_OK;
    });
}

mps_status_t MPS_CALL mps_server_config_set_bind_address(
    mps_server_config_t* config, const char* address, mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (const auto status = mutable_config(config, out);
            status != MPS_STATUS_OK) {
            return status;
        }
        if (!valid_text(address)) {
            diagnostic(out, 0, "bind address must be non-empty");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        config->value.bind_address = address;
        return MPS_STATUS_OK;
    });
}

mps_status_t MPS_CALL
mps_server_config_set_port(mps_server_config_t* config, uint16_t port,
                           mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (const auto status = mutable_config(config, out);
            status != MPS_STATUS_OK) {
            return status;
        }
        config->value.port = port;
        return MPS_STATUS_OK;
    });
}

#define MPS_SERVER_POSITIVE_SETTER(name, field)                                  \
  mps_status_t MPS_CALL name(mps_server_config_t *config, uint32_t value,        \
                             mps_diagnostic_t *out) {                            \
    return boundary(out, [&] {                                                   \
      if (const auto status = mutable_config(config, out);                       \
          status != MPS_STATUS_OK) {                                             \
        return status;                                                           \
      }                                                                          \
      if (value == 0U) {                                                         \
        diagnostic(out, 0, "value must be non-zero");                           \
        return MPS_STATUS_INVALID_ARGUMENT;                                      \
      }                                                                          \
      config->value.field = value;                                               \
      return MPS_STATUS_OK;                                                      \
    });                                                                          \
  }

MPS_SERVER_POSITIVE_SETTER(mps_server_config_set_max_connections,
                           max_connections)
MPS_SERVER_POSITIVE_SETTER(mps_server_config_set_max_channel_size,
                           max_channel_size)
MPS_SERVER_POSITIVE_SETTER(mps_server_config_set_max_message_size,
                           max_message_size)
MPS_SERVER_POSITIVE_SETTER(mps_server_config_set_output_queue_limit,
                           output_queue_limit)
MPS_SERVER_POSITIVE_SETTER(mps_server_config_set_command_queue_limit,
                           command_queue_limit)
MPS_SERVER_POSITIVE_SETTER(mps_server_config_set_shutdown_timeout_ms,
                           shutdown_timeout_ms)

#undef MPS_SERVER_POSITIVE_SETTER

mps_status_t MPS_CALL mps_server_config_set_log_callback(
    mps_server_config_t* config, mps_log_callback_t callback, void* user_data,
    mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (const auto status = mutable_config(config, out);
            status != MPS_STATUS_OK) {
            return status;
        }
        config->value.log_callback = callback;
        config->value.log_user_data = user_data;
        return MPS_STATUS_OK;
    });
}

void MPS_CALL mps_server_config_destroy(mps_server_config_t* config) {
    try {
        delete config;
    } catch (...) {
    }
}

mps_status_t MPS_CALL
mps_server_create(const mps_server_config_t* config, mps_server_t** out_server,
                  mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (config == nullptr || out_server == nullptr) {
            diagnostic(out, 0, "configuration or output server pointer is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        *out_server = nullptr;
        const auto payload = std::uint64_t{8} + config->value.max_channel_size +
                             config->value.max_message_size;
        if (payload > std::numeric_limits<std::uint32_t>::max()) {
            diagnostic(out, 0, "configured maximum payload overflows uint32_t");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        *out_server = new mps_server(config->value);
        const_cast<mps_server_config_t*>(config)->consumed = true;
        return MPS_STATUS_OK;
    });
}

mps_status_t MPS_CALL mps_server_start(mps_server_t* server,
                                       mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (server == nullptr) {
            diagnostic(out, 0, "server is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        std::string error;
        int system_code = 0;
        const auto status = server->runtime.start(error, system_code);
        if (status == MPS_STATUS_OK) {
            server->started = true;
        } else {
            diagnostic(out, system_code, error);
        }
        return status;
    });
}

mps_status_t MPS_CALL mps_server_bound_port(const mps_server_t* server,
                                            uint16_t* out_port,
                                            mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (server == nullptr || out_port == nullptr) {
            diagnostic(out, 0, "server or output port pointer is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        if (!server->started) {
            diagnostic(out, 0, "server has not started");
            return MPS_STATUS_INVALID_STATE;
        }
        *out_port = server->runtime.bound_port();
        return MPS_STATUS_OK;
    });
}

mps_status_t MPS_CALL mps_server_request_stop(mps_server_t* server,
                                              mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (server == nullptr) {
            diagnostic(out, 0, "server is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        server->runtime.request_stop();
        return MPS_STATUS_OK;
    });
}

mps_status_t MPS_CALL mps_server_wait(mps_server_t* server,
                                      mps_diagnostic_t* out) {
    return boundary(out, [&] {
        if (server == nullptr) {
            diagnostic(out, 0, "server is null");
            return MPS_STATUS_INVALID_ARGUMENT;
        }
        server->runtime.wait();
        return MPS_STATUS_OK;
    });
}

void MPS_CALL mps_server_destroy(mps_server_t* server) {
    try {
        delete server;
    } catch (...) {
    }
}

} // extern "C"