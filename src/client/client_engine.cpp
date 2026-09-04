#include "client/client_engine.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace mps::client
{
namespace
{

inline constexpr std::uint32_t max_channel_size = 1024U;
inline constexpr std::uint32_t max_message_size = 1048576U;
inline constexpr std::uint32_t max_payload_size =
    8U + max_channel_size + max_message_size;

bool send_all(int fd, const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto sent = ::send(fd, bytes.data() + offset, bytes.size() - offset,
                                 MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

class ClientEngine::Impl {
public:
    explicit Impl(Config value)
        : config(std::move(value)), decoder(max_payload_size) {
    }

    ~Impl() {
        request_stop();
        (void)wait();
    }

    enum class State { created, connecting, running, stopping, stopped };

    enum class Expected { ok, pong };

    mps_status_t connect(std::string& error, int& system_code) {
        State expected = State::created;
        if (!state.compare_exchange_strong(expected, State::connecting)) {
            error = "client is not in the created state";
            return MPS_STATUS_INVALID_STATE;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICSERV;
        addrinfo* addresses = nullptr;
        const auto service = std::to_string(config.port);
        const int lookup =
            ::getaddrinfo(config.host.c_str(), service.c_str(), &hints, &addresses);
        if (lookup != 0) {
            error = ::gai_strerror(lookup);
            system_code = lookup;
            state.store(State::stopped);
            return MPS_STATUS_IO_ERROR;
        }

        int connected = -1;
        mps_status_t failure_status = MPS_STATUS_IO_ERROR;
        int last_error = 0;
        for (auto* address = addresses; address != nullptr;
             address = address->ai_next) {
            const int candidate = ::socket(address->ai_family, address->ai_socktype,
                                           address->ai_protocol);
            if (candidate < 0) {
                last_error = errno;
                continue;
            }
            const int original_flags = ::fcntl(candidate, F_GETFL, 0);
            if (original_flags < 0 ||
                ::fcntl(candidate, F_SETFL, original_flags | O_NONBLOCK) < 0) {
                last_error = errno;
                ::close(candidate);
                continue;
            }
            int result = ::connect(candidate, address->ai_addr, address->ai_addrlen);
            if (result < 0 && errno == EINPROGRESS) {
                pollfd descriptor{candidate, POLLOUT, 0};
                do {
                    result = ::poll(&descriptor, 1,
                                    static_cast<int>(config.connect_timeout_ms));
                } while (result < 0 && errno == EINTR);
                if (result == 0) {
                    failure_status = MPS_STATUS_TIMEOUT;
                    last_error = ETIMEDOUT;
                } else if (result > 0) {
                    socklen_t size = sizeof(last_error);
                    if (::getsockopt(candidate, SOL_SOCKET, SO_ERROR, &last_error, &size) ==
                        0 &&
                        last_error == 0) {
                        result = 0;
                    } else {
                        result = -1;
                    }
                }
            }
            if (result == 0 &&
                ::fcntl(candidate, F_SETFL, original_flags) == 0) {
                connected = candidate;
                break;
            }
            if (last_error == 0) {
                last_error = errno;
            }
            ::close(candidate);
        }
        ::freeaddrinfo(addresses);
        if (connected < 0) {
            system_code = last_error;
            error = std::strerror(last_error);
            state.store(State::stopped);
            return failure_status;
        }

        State connecting = State::connecting;
        if (!state.compare_exchange_strong(connecting, State::running)) {
            ::close(connected);
            state.store(State::stopped);
            error = "connection was stopped while connecting";
            return MPS_STATUS_CLOSED;
        }
        socket.store(connected);
        timeval send_timeout{};
        send_timeout.tv_sec =
            static_cast<time_t>(config.shutdown_timeout_ms / 1000U);
        send_timeout.tv_usec = static_cast<suseconds_t>(
            (config.shutdown_timeout_ms % 1000U) * 1000U);
        (void)::setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                           sizeof(send_timeout));
        reader = std::thread([this] {
            reader_loop();
        });
        writer = std::thread([this] {
            writer_loop();
        });
        if (config.event_mode == MPS_EVENT_MODE_CALLBACK) {
            dispatcher = std::thread([this] {
                callback_loop();
            });
        }
        return MPS_STATUS_OK;
    }

    mps_status_t submit(protocol::Opcode opcode, std::vector<std::uint8_t> payload,
                        Expected expected, std::uint32_t& request_id) {
        std::lock_guard lock(command_mutex);
        if (state.load() != State::running) {
            return MPS_STATUS_INVALID_STATE;
        }
        if (commands.size() >= config.command_queue_limit ||
            in_flight.size() >= config.command_queue_limit) {
            return MPS_STATUS_WOULD_BLOCK;
        }
        const auto encoded_size = protocol::header_size + payload.size();
        if (encoded_size > config.command_queue_bytes_limit -
            std::min<std::size_t>(
                command_bytes,
                config.command_queue_bytes_limit)) {
            return MPS_STATUS_WOULD_BLOCK;
        }
        std::uint32_t candidate = next_request_id;
        do {
            if (candidate == 0U) {
                candidate = 1U;
            }
            if (!in_flight.contains(candidate)) {
                break;
            }
            ++candidate;
        } while (candidate != next_request_id);
        if (in_flight.contains(candidate)) {
            return MPS_STATUS_WOULD_BLOCK;
        }
        next_request_id = candidate == std::numeric_limits<std::uint32_t>::max()
                              ? 1U
                              : candidate + 1U;
        auto frame = protocol::encode_frame(static_cast<std::uint8_t>(opcode),
                                            candidate, payload);
        commands.push_back(std::move(frame));
        try {
            in_flight.emplace(candidate, expected);
        } catch (...) {
            commands.pop_back();
            throw;
        }
        command_bytes += encoded_size;
        request_id = candidate;
        command_cv.notify_one();
        return MPS_STATUS_OK;
    }

    mps_status_t poll(std::uint32_t timeout_ms, std::unique_ptr<Event>& event) {
        if (config.event_mode != MPS_EVENT_MODE_POLL) {
            return MPS_STATUS_WRONG_EVENT_MODE;
        }
        std::unique_lock poll_lock(poll_mutex, std::try_to_lock);
        if (!poll_lock.owns_lock()) {
            return MPS_STATUS_INVALID_STATE;
        }
        std::unique_lock lock(event_mutex);
        const auto ready = event_cv.wait_for(
            lock, std::chrono::milliseconds(timeout_ms),
            [this] {
                return !events.empty() || state.load() == State::stopped;
            });
        if (!ready || events.empty()) {
            return state.load() == State::stopped
                       ? MPS_STATUS_CLOSED
                       : MPS_STATUS_TIMEOUT;
        }
        event = std::make_unique<Event>(std::move(events.front()));
        events.pop_front();
        event_bytes -= sizeof(Event) + event->channel.size() + event->message.size() +
            event->error_message.size();
        return MPS_STATUS_OK;
    }

    void request_stop() noexcept {
        {
            std::lock_guard lock(command_mutex);
            auto current = state.load();
            while (current == State::running || current == State::connecting) {
                if (state.compare_exchange_weak(current, State::stopping)) {
                    stop_requested.store(true);
                    command_cv.notify_all();
                    return;
                }
            }
            if (current == State::created) {
                state.store(State::stopped);
                stop_requested.store(true);
                event_cv.notify_all();
            }
        }
    }

    mps_status_t wait() noexcept {
        if ((dispatcher.joinable() &&
             dispatcher.get_id() == std::this_thread::get_id()) ||
            (reader.joinable() && reader.get_id() == std::this_thread::get_id()) ||
            (writer.joinable() && writer.get_id() == std::this_thread::get_id())) {
            return MPS_STATUS_INVALID_STATE;
        }
        std::lock_guard lock(wait_mutex);
        request_stop();
        std::mutex deadline_mutex;
        std::condition_variable deadline_cv;
        bool writer_finished = false;
        std::thread deadline([&] {
            std::unique_lock deadline_lock(deadline_mutex);
            if (!deadline_cv.wait_for(
                deadline_lock,
                std::chrono::milliseconds(config.shutdown_timeout_ms),
                [&] {
                    return writer_finished;
                })) {
                const int fd = socket.load();
                if (fd >= 0) {
                    ::shutdown(fd, SHUT_RDWR);
                }
            }
        });
        if (writer.joinable()) {
            writer.join();
        }
        {
            std::lock_guard deadline_lock(deadline_mutex);
            writer_finished = true;
        }
        deadline_cv.notify_one();
        deadline.join();
        const int fd = socket.load();
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
        if (reader.joinable()) {
            reader.join();
        }
        event_cv.notify_all();
        if (dispatcher.joinable()) {
            dispatcher.join();
        }
        const int old_fd = socket.exchange(-1);
        if (old_fd >= 0) {
            ::close(old_fd);
        }
        state.store(State::stopped);
        event_cv.notify_all();
        return MPS_STATUS_OK;
    }

    void force_close(mps_disconnect_cause_t cause) noexcept {
        disconnect_cause.store(cause);
        stop_requested.store(true);
        state.store(State::stopping);
        command_cv.notify_all();
        const int fd = socket.load();
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    bool push_event(Event event) noexcept {
        try {
            std::lock_guard lock(event_mutex);
            const auto event_size = sizeof(Event) + event.channel.size() +
                                    event.message.size() + event.error_message.size();
            if (events.size() >= config.event_queue_limit ||
                event_size > config.event_queue_bytes_limit -
                std::min<std::size_t>(
                    event_bytes, config.event_queue_bytes_limit)) {
                return false;
            }
            events.push_back(std::move(event));
            event_bytes += event_size;
            event_cv.notify_one();
            return true;
        } catch (...) {
            return false;
        }
    }

    void report_disconnected(mps_disconnect_cause_t cause) noexcept {
        if (disconnected_reported.exchange(true)) {
            return;
        }
        Event event;
        event.kind = MPS_EVENT_DISCONNECTED;
        event.disconnect_cause = cause;
        if (!push_event(std::move(event))) {
            try {
                std::lock_guard lock(event_mutex);
                events.clear();
                event_bytes = 0U;
                Event replacement;
                replacement.kind = MPS_EVENT_DISCONNECTED;
                replacement.disconnect_cause = MPS_DISCONNECT_QUEUE_LIMIT;
                events.push_back(std::move(replacement));
                event_bytes = sizeof(Event);
                event_cv.notify_all();
            } catch (...) {
            }
        }
    }

    bool correlate(std::uint32_t id, Expected expected) {
        std::lock_guard lock(command_mutex);
        const auto found = in_flight.find(id);
        if (id == 0U || found == in_flight.end() || found->second != expected) {
            return false;
        }
        in_flight.erase(found);
        return true;
    }

    bool correlate_error(std::uint32_t id) {
        std::lock_guard lock(command_mutex);
        const auto found = in_flight.find(id);
        if (id == 0U || found == in_flight.end()) {
            return false;
        }
        in_flight.erase(found);
        return true;
    }

    bool handle_frame(const protocol::Frame& frame) {
        if (frame.flags != 0U) {
            return false;
        }
        Event event;
        event.request_id = frame.request_id;
        switch (static_cast<protocol::Opcode>(frame.opcode)) {
        case protocol::Opcode::ok:
            if (!frame.payload.empty() || !correlate(frame.request_id, Expected::ok)) {
                return false;
            }
            event.kind = MPS_EVENT_OK;
            break;
        case protocol::Opcode::pong:
            if (!frame.payload.empty() ||
                !correlate(frame.request_id, Expected::pong)) {
                return false;
            }
            event.kind = MPS_EVENT_PONG;
            break;
        case protocol::Opcode::message: {
            if (frame.request_id != 0U) {
                return false;
            }
            protocol::ChannelMessage message;
            protocol::ErrorCode error{};
            if (!protocol::decode_channel_message(frame.payload, max_channel_size,
                                                  max_message_size, message, error)) {
                return false;
            }
            event.kind = MPS_EVENT_MESSAGE;
            event.channel = std::move(message.channel);
            event.message = std::move(message.message);
            break;
        }
        case protocol::Opcode::error: {
            protocol::RemoteError remote;
            if (!correlate_error(frame.request_id) ||
                !protocol::decode_error(frame.payload, 4096U, remote)) {
                return false;
            }
            event.kind = MPS_EVENT_REMOTE_ERROR;
            event.protocol_error = static_cast<mps_protocol_error_t>(remote.code);
            event.error_message = std::move(remote.message);
            break;
        }
        case protocol::Opcode::publish:
        case protocol::Opcode::subscribe:
        case protocol::Opcode::unsubscribe:
        case protocol::Opcode::ping:
        default:
            return false;
        }
        if (!push_event(std::move(event))) {
            force_close(MPS_DISCONNECT_QUEUE_LIMIT);
            return false;
        }
        return true;
    }

    void reader_loop() noexcept {
        mps_disconnect_cause_t cause = MPS_DISCONNECT_REMOTE_CLOSED;
        try {
            std::array<std::uint8_t, 16384> buffer{};
            for (;;) {
                const auto received =
                    ::recv(socket.load(), buffer.data(), buffer.size(), 0);
                if (received > 0) {
                    auto batch = decoder.consume(std::span(
                        buffer.data(), static_cast<std::size_t>(received)));
                    if (batch.status != protocol::DecodeStatus::ok) {
                        cause = MPS_DISCONNECT_PROTOCOL_ERROR;
                        break;
                    }
                    bool valid = true;
                    for (const auto& frame : batch.frames) {
                        if (!handle_frame(frame)) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) {
                        cause = disconnect_cause.load() == 0U
                                    ? MPS_DISCONNECT_PROTOCOL_ERROR
                                    : disconnect_cause.load();
                        break;
                    }
                    continue;
                }
                if (received < 0 && errno == EINTR) {
                    continue;
                }
                if (received < 0 && !stop_requested.load()) {
                    cause = MPS_DISCONNECT_IO_ERROR;
                }
                break;
            }
        } catch (...) {
            cause = MPS_DISCONNECT_IO_ERROR;
        }
        if (stop_requested.load() && disconnect_cause.load() == 0U) {
            cause = MPS_DISCONNECT_REQUESTED;
        }
        stop_requested.store(true);
        state.store(State::stopped);
        command_cv.notify_all();
        const int fd = socket.load();
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
        report_disconnected(cause);
    }

    void writer_loop() noexcept {
        try {
            for (;;) {
                protocol::SharedFrame frame;
                {
                    std::unique_lock lock(command_mutex);
                    command_cv.wait(lock, [this] {
                        return stop_requested.load() || !commands.empty();
                    });
                    if (commands.empty()) {
                        break;
                    }
                    frame = std::move(commands.front());
                    commands.pop_front();
                    command_bytes -= frame->size();
                }
                if (!send_all(socket.load(), *frame)) {
                    force_close(MPS_DISCONNECT_IO_ERROR);
                    return;
                }
            }
            const int fd = socket.load();
            if (fd >= 0) {
                ::shutdown(fd, SHUT_WR);
            }
        } catch (...) {
            force_close(MPS_DISCONNECT_IO_ERROR);
        }
    }

    void callback_loop() noexcept {
        try {
            for (;;) {
                Event event;
                {
                    std::unique_lock lock(event_mutex);
                    event_cv.wait(lock, [this] {
                        return !events.empty() || state.load() == State::stopped;
                    });
                    if (events.empty()) {
                        break;
                    }
                    event = std::move(events.front());
                    events.pop_front();
                    event_bytes -= sizeof(Event) + event.channel.size() +
                        event.message.size() + event.error_message.size();
                }
                config.callback(event);
                if (event.kind == MPS_EVENT_DISCONNECTED) {
                    break;
                }
            }
        } catch (...) {
            force_close(MPS_DISCONNECT_IO_ERROR);
        }
    }

    Config config;
    protocol::FrameDecoder decoder;
    std::atomic<State> state{State::created};
    std::atomic<int> socket{-1};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> disconnected_reported{false};
    std::atomic<mps_disconnect_cause_t> disconnect_cause{0};
    std::thread reader;
    std::thread writer;
    std::thread dispatcher;
    std::mutex wait_mutex;
    std::mutex command_mutex;
    std::condition_variable command_cv;
    std::deque<protocol::SharedFrame> commands;
    std::size_t command_bytes{0};
    std::unordered_map<std::uint32_t, Expected> in_flight;
    std::uint32_t next_request_id{1U};
    std::mutex event_mutex;
    std::condition_variable event_cv;
    std::deque<Event> events;
    std::size_t event_bytes{0};
    std::mutex poll_mutex;
};

ClientEngine::ClientEngine(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
}

ClientEngine::~ClientEngine() = default;

mps_status_t ClientEngine::connect(std::string& error, int& system_code) {
    return impl_->connect(error, system_code);
}

mps_status_t ClientEngine::publish(std::string_view channel,
                                   std::span<const std::uint8_t> message,
                                   std::uint32_t& request_id) {
    if (channel.empty() || channel.size() > max_channel_size ||
        message.size() > max_message_size) {
        return MPS_STATUS_INVALID_ARGUMENT;
    }
    return impl_->submit(protocol::Opcode::publish,
                         protocol::encode_channel_message(channel, message),
                         Impl::Expected::ok, request_id);
}

mps_status_t ClientEngine::subscribe(std::string_view channel,
                                     std::uint32_t& request_id) {
    if (channel.empty() || channel.size() > max_channel_size) {
        return MPS_STATUS_INVALID_ARGUMENT;
    }
    return impl_->submit(protocol::Opcode::subscribe,
                         protocol::encode_channel(channel), Impl::Expected::ok,
                         request_id);
}

mps_status_t ClientEngine::unsubscribe(std::string_view channel,
                                       std::uint32_t& request_id) {
    if (channel.empty() || channel.size() > max_channel_size) {
        return MPS_STATUS_INVALID_ARGUMENT;
    }
    return impl_->submit(protocol::Opcode::unsubscribe,
                         protocol::encode_channel(channel), Impl::Expected::ok,
                         request_id);
}

mps_status_t ClientEngine::ping(std::uint32_t& request_id) {
    return impl_->submit(protocol::Opcode::ping, {}, Impl::Expected::pong,
                         request_id);
}

mps_status_t ClientEngine::poll(std::uint32_t timeout_ms,
                                std::unique_ptr<Event>& event) {
    return impl_->poll(timeout_ms, event);
}

void ClientEngine::request_stop() noexcept {
    impl_->request_stop();
}

mps_status_t ClientEngine::wait() noexcept {
    return impl_->wait();
}

mps_event_delivery_mode_t ClientEngine::event_mode() const noexcept {
    return impl_->config.event_mode;
}

} // namespace mps::client