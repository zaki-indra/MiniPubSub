#include "cli/client_command.hpp"
#include "minipubsub/minipubsub.h"

#include <poll.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cerrno>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace
{
volatile std::sig_atomic_t stopping = 0;

void stop_signal(int) { stopping = 1; }

mps_bytes_view_t view(std::string_view value) {
    return {reinterpret_cast<const std::uint8_t*>(value.data()),
            static_cast<std::uint32_t>(value.size())};
}

std::string_view event_bytes(mps_bytes_view_t value) {
    return value.length == 0U
        ? std::string_view{}
        : std::string_view{reinterpret_cast<const char*>(value.data), value.length};
}

void usage(std::ostream& output) {
    output << "usage: minipubsub-cli [--host HOST] [--port PORT] "
              "[ping|publish CHANNEL MESSAGE|subscribe CHANNEL|"
              "unsubscribe CHANNEL]\n";
}

void help() {
    std::cout << "Commands:\n"
                 "  PING\n"
                 "  PUBLISH <channel> <message>\n"
                 "  SUBSCRIBE <channel>\n"
                 "  UNSUBSCRIBE <channel>\n"
                 "  HELP\n"
                 "  EXIT | QUIT\n"
                 "Arguments may use single or double quotes. Escapes include "
                 "\\n, \\r, \\t, \\0, and \\xHH.\n";
}

std::string status_name(mps_status_t status) {
    switch (status) {
    case MPS_STATUS_INVALID_ARGUMENT: return "Invalid Argument";
    case MPS_STATUS_OUT_OF_MEMORY: return "Out Of Memory";
    case MPS_STATUS_INVALID_STATE: return "Invalid State";
    case MPS_STATUS_TIMEOUT: return "Timeout";
    case MPS_STATUS_WOULD_BLOCK: return "Would Block";
    case MPS_STATUS_IO_ERROR: return "I/O Error";
    case MPS_STATUS_CLOSED: return "Closed";
    case MPS_STATUS_WRONG_EVENT_MODE: return "Wrong Event Mode";
    default: return "Internal Error";
    }
}

std::string diagnostic_error(mps_status_t status,
                             const mps_diagnostic_t& diagnostic) {
    return diagnostic.message[0] != '\0' ? diagnostic.message : status_name(status);
}

std::string disconnect_name(mps_disconnect_cause_t cause) {
    switch (cause) {
    case MPS_DISCONNECT_REQUESTED: return "requested";
    case MPS_DISCONNECT_REMOTE_CLOSED: return "remote closed";
    case MPS_DISCONNECT_IO_ERROR: return "I/O error";
    case MPS_DISCONNECT_PROTOCOL_ERROR: return "protocol error";
    case MPS_DISCONNECT_QUEUE_LIMIT: return "queue limit";
    default: return "unknown cause";
    }
}

struct Options {
    std::string host{"127.0.0.1"};
    std::uint16_t port{7379U};
    int command_index{0};
    bool show_help{false};
};

std::optional<Options> parse_options(int argc, char** argv) {
    Options result;
    int index = 1;
    while (index < argc) {
        const std::string_view argument = argv[index];
        if (argument == "--help") {
            result.show_help = true;
            ++index;
            break;
        }
        if (argument != "--host" && argument != "--port") {
            break;
        }
        if (++index >= argc) {
            return std::nullopt;
        }
        if (argument == "--host") {
            result.host = argv[index++];
        } else {
            const std::string_view text = argv[index++];
            unsigned int value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                                value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                value == 0U || value > 65535U) {
                return std::nullopt;
            }
            result.port = static_cast<std::uint16_t>(value);
        }
    }
    result.command_index = index;
    if (result.show_help && index != argc) {
        return std::nullopt;
    }
    return result;
}

mps_status_t submit(mps_client_t* client, const mps::cli::Command& command,
                    mps_request_id_t& request, mps_diagnostic_t& diagnostic) {
    switch (command.kind) {
    case mps::cli::CommandKind::ping:
        return mps_client_ping(client, &request, &diagnostic);
    case mps::cli::CommandKind::publish: {
        const auto channel = view(command.arguments[0]);
        const auto message = view(command.arguments[1]);
        return mps_client_publish(client, &channel, &message, &request, &diagnostic);
    }
    case mps::cli::CommandKind::subscribe: {
        const auto channel = view(command.arguments[0]);
        return mps_client_subscribe(client, &channel, &request, &diagnostic);
    }
    case mps::cli::CommandKind::unsubscribe: {
        const auto channel = view(command.arguments[0]);
        return mps_client_unsubscribe(client, &channel, &request, &diagnostic);
    }
    case mps::cli::CommandKind::help:
    case mps::cli::CommandKind::exit:
        return MPS_STATUS_INVALID_ARGUMENT;
    }
    return MPS_STATUS_INTERNAL_ERROR;
}

struct InteractiveState {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<mps_request_id_t> pending;
    mps_event_kind_t response_kind{0};
    std::string response_error;
    bool response_ready{false};
    bool disconnected{false};
    mps_disconnect_cause_t disconnect_cause{0};
    bool prompt_visible{false};
};

void interactive_callback(const mps_event_t* event, void* opaque) {
    auto& state = *static_cast<InteractiveState*>(opaque);
    const auto kind = mps_event_kind(event);
    if (kind == MPS_EVENT_MESSAGE) {
        mps_bytes_view_t channel{};
        mps_bytes_view_t message{};
        (void)mps_event_channel(event, &channel);
        (void)mps_event_message(event, &message);
        const auto output = "!msg " + mps::cli::escape_bytes(event_bytes(channel), true) +
            ": " + mps::cli::escape_bytes(event_bytes(message), false);
        std::lock_guard lock(state.mutex);
        if (state.prompt_visible) {
            std::cout << "\r\n";
        }
        std::cout << output << '\n';
        if (state.prompt_visible) {
            std::cout << "> ";
        }
        std::cout.flush();
        return;
    }
    std::lock_guard lock(state.mutex);
    if (kind == MPS_EVENT_DISCONNECTED) {
        state.disconnected = true;
        state.disconnect_cause = mps_event_disconnect_cause(event);
        state.cv.notify_all();
        return;
    }
    if (state.pending && mps_event_request_id(event) == *state.pending &&
        (kind == MPS_EVENT_OK || kind == MPS_EVENT_PONG ||
         kind == MPS_EVENT_REMOTE_ERROR)) {
        state.response_kind = kind;
        state.response_error = kind == MPS_EVENT_REMOTE_ERROR
            ? mps_event_error_message(event) : "";
        state.response_ready = true;
        state.cv.notify_all();
    }
}

enum class ReadResult { line, eof, stopped, disconnected, error };

ReadResult read_line(InteractiveState& state, std::string& line) {
    line.clear();
    for (;;) {
        if (stopping != 0) {
            return ReadResult::stopped;
        }
        {
            std::lock_guard lock(state.mutex);
            if (state.disconnected) {
                return ReadResult::disconnected;
            }
        }
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&input, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ReadResult::error;
        }
        if (ready == 0) {
            continue;
        }
        char character = '\0';
        const auto count = ::read(STDIN_FILENO, &character, 1U);
        if (count == 0) {
            return line.empty() ? ReadResult::eof : ReadResult::line;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ReadResult::error;
        }
        if (character == '\n') {
            return ReadResult::line;
        }
        if (character != '\r') {
            line.push_back(character);
        }
    }
}

mps_client_t* create_client(const Options& options, mps_event_callback_t callback,
                            void* context, mps_diagnostic_t& diagnostic,
                            mps_status_t& failure_status) {
    mps_client_config_t* config = nullptr;
    mps_client_t* client = nullptr;
    mps_status_t status = mps_client_config_create(&config, &diagnostic);
    if (status == MPS_STATUS_OK) {
        status = mps_client_config_set_host(config, options.host.c_str(), &diagnostic);
    }
    if (status == MPS_STATUS_OK) {
        status = mps_client_config_set_port(config, options.port, &diagnostic);
    }
    if (status == MPS_STATUS_OK && callback != nullptr) {
        status = mps_client_config_set_event_callback(config, callback, context,
                                                      &diagnostic);
    }
    if (status == MPS_STATUS_OK) {
        status = mps_client_create(config, &client, &diagnostic);
    }
    mps_client_config_destroy(config);
    if (status != MPS_STATUS_OK) {
        failure_status = status;
        return nullptr;
    }
    status = mps_client_connect(client, &diagnostic);
    if (status != MPS_STATUS_OK) {
        failure_status = status;
        mps_client_destroy(client);
        return nullptr;
    }
    return client;
}

void stop_client(mps_client_t* client) {
    (void)mps_client_request_stop(client, nullptr);
    (void)mps_client_wait(client, nullptr);
    mps_client_destroy(client);
}

int interactive(const Options& options) {
    InteractiveState state;
    mps_diagnostic_t diagnostic{};
    mps_status_t failure_status = MPS_STATUS_OK;
    auto* client = create_client(options, interactive_callback, &state, diagnostic,
                                 failure_status);
    if (client == nullptr) {
        std::cerr << "ERROR: " << diagnostic_error(failure_status, diagnostic) << '\n';
        return 1;
    }
    bool normal_exit = false;
    int result = 0;
    for (;;) {
        {
            std::lock_guard lock(state.mutex);
            state.prompt_visible = true;
            std::cout << "> " << std::flush;
        }
        std::string line;
        const auto read_result = read_line(state, line);
        {
            std::lock_guard lock(state.mutex);
            state.prompt_visible = false;
        }
        if (read_result == ReadResult::eof || read_result == ReadResult::stopped) {
            normal_exit = true;
            break;
        }
        if (read_result == ReadResult::disconnected) {
            std::lock_guard lock(state.mutex);
            std::cout << "\nERROR: Disconnected ("
                      << disconnect_name(state.disconnect_cause) << ")\n";
            result = 1;
            break;
        }
        if (read_result == ReadResult::error) {
            std::cout << "\nERROR: Failed to read input\n";
            result = 1;
            break;
        }
        const auto parsed = mps::cli::parse_command(line);
        if (parsed.empty) {
            continue;
        }
        if (!parsed.valid) {
            std::cout << "ERROR: Invalid Argument\n";
            continue;
        }
        if (parsed.command.kind == mps::cli::CommandKind::help) {
            help();
            continue;
        }
        if (parsed.command.kind == mps::cli::CommandKind::exit) {
            normal_exit = true;
            break;
        }
        mps_status_t status = MPS_STATUS_OK;
        {
            std::unique_lock lock(state.mutex);
            state.response_ready = false;
            state.response_error.clear();
            mps_request_id_t request = 0;
            diagnostic = {};
            status = submit(client, parsed.command, request, diagnostic);
            if (status == MPS_STATUS_OK) {
                state.pending = request;
                const auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(30);
                while (!state.response_ready && !state.disconnected &&
                       stopping == 0 &&
                       std::chrono::steady_clock::now() < deadline) {
                    state.cv.wait_for(lock, std::chrono::milliseconds(100));
                }
                if (!state.response_ready && !state.disconnected && stopping == 0) {
                    std::cout << "ERROR: Timeout\n";
                    result = 1;
                    break;
                }
                if (stopping != 0) {
                    normal_exit = true;
                    break;
                }
                if (state.disconnected) {
                    std::cout << "ERROR: Disconnected ("
                              << disconnect_name(state.disconnect_cause) << ")\n";
                    result = 1;
                    break;
                }
                if (state.response_kind == MPS_EVENT_REMOTE_ERROR) {
                    std::cout << "ERROR: " << state.response_error << '\n';
                } else {
                    std::cout << (state.response_kind == MPS_EVENT_PONG ? "PONG" : "OK")
                              << '\n';
                }
                state.pending.reset();
            }
        }
        if (status != MPS_STATUS_OK) {
            std::cout << "ERROR: " << diagnostic_error(status, diagnostic) << '\n';
        }
    }
    stop_client(client);
    if (normal_exit) {
        std::cout << "bye. :)\n";
    }
    return result;
}

int one_shot(const Options& options, int argc, char** argv) {
    std::string line = argv[options.command_index];
    for (int index = options.command_index + 1; index < argc; ++index) {
        line.push_back(' ');
        line += mps::cli::escape_bytes(argv[index], true);
    }
    const auto parsed = mps::cli::parse_command(line);
    if (!parsed.valid || parsed.command.kind == mps::cli::CommandKind::help ||
        parsed.command.kind == mps::cli::CommandKind::exit) {
        std::cerr << "invalid command arguments\n";
        return 2;
    }
    mps_diagnostic_t diagnostic{};
    mps_status_t failure_status = MPS_STATUS_OK;
    auto* client = create_client(options, nullptr, nullptr, diagnostic,
                                 failure_status);
    if (client == nullptr) {
        std::cerr << diagnostic_error(failure_status, diagnostic) << '\n';
        return 1;
    }
    mps_request_id_t request = 0;
    auto status = submit(client, parsed.command, request, diagnostic);
    if (status != MPS_STATUS_OK) {
        std::cerr << diagnostic_error(status, diagnostic) << '\n';
        mps_client_destroy(client);
        return 1;
    }
    for (;;) {
        mps_event_t* event = nullptr;
        status = mps_client_poll_event(client, 30000U, &event, &diagnostic);
        if (status != MPS_STATUS_OK) {
            std::cerr << diagnostic_error(status, diagnostic) << '\n';
            mps_client_destroy(client);
            return 1;
        }
        const auto kind = mps_event_kind(event);
        if (kind == MPS_EVENT_MESSAGE) {
            mps_bytes_view_t channel{};
            mps_bytes_view_t message{};
            (void)mps_event_channel(event, &channel);
            (void)mps_event_message(event, &message);
            std::cout << mps::cli::escape_bytes(event_bytes(channel), false) << ' '
                      << mps::cli::escape_bytes(event_bytes(message), false) << '\n';
        } else if (kind == MPS_EVENT_REMOTE_ERROR &&
                   mps_event_request_id(event) == request) {
            std::cerr << mps_event_error_message(event) << '\n';
            mps_event_destroy(event);
            mps_client_destroy(client);
            return 1;
        } else if (kind == MPS_EVENT_DISCONNECTED) {
            std::cerr << "disconnected ("
                      << disconnect_name(mps_event_disconnect_cause(event)) << ")\n";
            mps_event_destroy(event);
            mps_client_destroy(client);
            return 1;
        } else if (mps_event_request_id(event) == request) {
            std::cout << (kind == MPS_EVENT_PONG ? "PONG" : "OK") << '\n';
            mps_event_destroy(event);
            if (parsed.command.kind != mps::cli::CommandKind::subscribe) {
                break;
            }
            continue;
        }
        mps_event_destroy(event);
    }
    mps_client_destroy(client);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    if (!options) {
        usage(std::cerr);
        return 2;
    }
    if (options->show_help) {
        usage(std::cout);
        help();
        return 0;
    }
    if (options->command_index == argc) {
        std::signal(SIGINT, stop_signal);
        std::signal(SIGTERM, stop_signal);
        return interactive(*options);
    }
    return one_shot(*options, argc, argv);
}
