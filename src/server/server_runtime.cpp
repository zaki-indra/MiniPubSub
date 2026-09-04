#include "server/server_runtime.hpp"

#include "protocol/codec.hpp"

#include <arpa/inet.h>
#include <netdb.h>
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
#include <vector>

namespace mps::server
{
namespace
{

std::uint32_t maximum_payload(const Config& config) {
    const auto publish = std::uint64_t{8} + config.max_channel_size +
                         config.max_message_size;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(publish,
                                std::numeric_limits<std::uint32_t>::max()));
}

bool send_all(const int fd, const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto sent = send(fd, bytes.data() + offset, bytes.size() - offset,
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

class ServerRuntime::Impl : public core::DeliverySink {
public:
    explicit Impl(Config value)
        : config(std::move(value)),
          broker(*this, config.max_channel_size, config.max_message_size) {
    }

    ~Impl() override {
        wait();
    }

    struct Command {
        enum class Kind { opened, frame, closed } kind;

        core::ConnectionId id{};
        protocol::Frame frame;

        std::size_t retained_bytes() const noexcept {
            return sizeof(Command) + frame.payload.size();
        }
    };

    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        Connection(Impl& owner, core::ConnectionId id, int socket)
            : owner_(owner),
              id_(id),
              socket_(socket),
              decoder_(maximum_payload(owner.config)) {
            timeval send_timeout{};
            send_timeout.tv_sec =
                static_cast<time_t>(owner.config.shutdown_timeout_ms / 1000U);
            send_timeout.tv_usec = static_cast<suseconds_t>(
                (owner.config.shutdown_timeout_ms % 1000U) * 1000U);
            (void)::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                               sizeof(send_timeout));
        }

        ~Connection() {
            stop();
            join();
            const int fd = socket_.exchange(-1);
            if (fd >= 0) {
                ::close(fd);
            }
        }

        void start() {
            const auto self = shared_from_this();
            reader_ = std::thread([self] {
                self->read_loop();
            });
            writer_ = std::thread([self] {
                self->write_loop();
            });
        }

        bool enqueue(protocol::SharedFrame frame) {
            std::lock_guard lock(output_mutex_);
            if (stopping_.load()) {
                return false;
            }
            if (frame->size() > owner_.config.output_queue_limit -
                std::min<std::size_t>(
                    output_bytes_, owner_.config.output_queue_limit)) {
                return false;
            }
            output_bytes_ += frame->size();
            output_.push_back(std::move(frame));
            output_cv_.notify_one();
            return true;
        }

        void drain_and_stop() noexcept {
            stop_reading();
            drain_output();
        }

        void stop_reading() noexcept {
            reader_stopping_.store(true);
            const int fd = socket_.load();
            if (fd >= 0) {
                ::shutdown(fd, SHUT_RD);
            }
        }

        void drain_output() noexcept {
            draining_.store(true);
            output_cv_.notify_all();
        }

        void stop() noexcept {
            if (!stopping_.exchange(true)) {
                const int fd = socket_.load();
                if (fd >= 0) {
                    ::shutdown(fd, SHUT_RDWR);
                }
                output_cv_.notify_all();
            }
        }

        void join() noexcept {
            join_reader();
            join_writer();
        }

        void join_reader() noexcept {
            if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) {
                reader_.join();
            }
        }

        void join_writer() noexcept {
            if (writer_.joinable() && writer_.get_id() != std::this_thread::get_id()) {
                writer_.join();
            }
        }

    private:
        void notify_closed() noexcept {
            if (!closed_reported_.exchange(true)) {
                owner_.submit(Command{Command::Kind::closed, id_, {}});
                owner_.active_connections.fetch_sub(1U);
                owner_.log(MPS_LOG_INFO, id_, "transport", "connection_closed",
                           "connection closed");
            }
        }

        void thread_finished() noexcept {
            if (live_threads_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
                const int fd = socket_.exchange(-1);
                if (fd >= 0) {
                    ::close(fd);
                }
            }
        }

        void read_loop() noexcept {
            try {
                std::array<std::uint8_t, 16384> buffer{};
                while (!stopping_.load()) {
                    const auto received =
                        ::recv(socket_.load(), buffer.data(), buffer.size(), 0);
                    if (received > 0) {
                        auto batch = decoder_.consume(std::span(
                            buffer.data(), static_cast<std::size_t>(received)));
                        if (batch.status != protocol::DecodeStatus::ok) {
                            owner_.log(MPS_LOG_WARNING, id_, "protocol",
                                       "fatal_frame_error", "invalid frame header");
                            break;
                        }
                        for (auto& frame : batch.frames) {
                            if (!owner_.submit(Command{Command::Kind::frame, id_,
                                                       std::move(frame)})) {
                                break;
                            }
                        }
                        continue;
                    }
                    if (received < 0 && errno == EINTR) {
                        continue;
                    }
                    break;
                }
            } catch (...) {
                owner_.log(MPS_LOG_ERROR, id_, "transport", "reader_exception",
                           "connection reader failed");
            }
            if (reader_stopping_.load()) {
                notify_closed();
            } else {
                stop();
                notify_closed();
            }
            thread_finished();
        }

        void write_loop() noexcept {
            try {
                for (;;) {
                    protocol::SharedFrame frame;
                    {
                        std::unique_lock lock(output_mutex_);
                        output_cv_.wait(lock, [this] {
                            return stopping_.load() || draining_.load() || !output_.empty();
                        });
                        if (output_.empty()) {
                            break;
                        }
                        frame = std::move(output_.front());
                        output_.pop_front();
                        output_bytes_ -= frame->size();
                    }
                    if (!send_all(socket_.load(), *frame)) {
                        break;
                    }
                }
            } catch (...) {
                owner_.log(MPS_LOG_ERROR, id_, "transport", "writer_exception",
                           "connection writer failed");
            }
            stop();
            notify_closed();
            thread_finished();
        }

        Impl& owner_;
        core::ConnectionId id_;
        std::atomic<int> socket_;
        protocol::FrameDecoder decoder_;
        std::atomic<bool> stopping_{false};
        std::atomic<bool> reader_stopping_{false};
        std::atomic<bool> draining_{false};
        std::atomic<bool> closed_reported_{false};
        std::atomic<std::uint32_t> live_threads_{2U};
        std::mutex output_mutex_;
        std::condition_variable output_cv_;
        std::deque<protocol::SharedFrame> output_;
        std::size_t output_bytes_{0};
        std::thread reader_;
        std::thread writer_;
    };

    mps_status_t start(std::string& error, int& system_code) {
        State expected = State::created;
        if (!state.compare_exchange_strong(expected, State::starting)) {
            error = "server is not in the created state";
            return MPS_STATUS_INVALID_STATE;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICSERV;
        addrinfo* addresses = nullptr;
        const auto service = std::to_string(config.port);
        const int lookup = ::getaddrinfo(config.bind_address.c_str(), service.c_str(),
                                         &hints, &addresses);
        if (lookup != 0) {
            error = ::gai_strerror(lookup);
            system_code = lookup;
            state.store(State::stopped);
            return MPS_STATUS_IO_ERROR;
        }

        int candidate = -1;
        int last_error = 0;
        for (auto* address = addresses; address != nullptr;
             address = address->ai_next) {
            candidate = ::socket(address->ai_family, address->ai_socktype,
                                 address->ai_protocol);
            if (candidate < 0) {
                last_error = errno;
                continue;
            }
            const int enabled = 1;
            (void)::setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &enabled,
                               sizeof(enabled));
            if (::bind(candidate, address->ai_addr, address->ai_addrlen) == 0 &&
                ::listen(candidate, SOMAXCONN) == 0) {
                break;
            }
            last_error = errno;
            ::close(candidate);
            candidate = -1;
        }
        ::freeaddrinfo(addresses);
        if (candidate < 0) {
            error = std::strerror(last_error);
            system_code = last_error;
            state.store(State::stopped);
            return MPS_STATUS_IO_ERROR;
        }

        sockaddr_storage local{};
        socklen_t local_size = sizeof(local);
        if (::getsockname(candidate, reinterpret_cast<sockaddr*>(&local),
                          &local_size) != 0) {
            system_code = errno;
            error = std::strerror(errno);
            ::close(candidate);
            state.store(State::stopped);
            return MPS_STATUS_IO_ERROR;
        }
        if (local.ss_family == AF_INET) {
            bound_port_value = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
        } else {
            bound_port_value =
                ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
        }
        state.store(State::running);
        listener.store(candidate);
        broker_thread = std::thread([this] {
            broker_loop();
        });
        accept_thread = std::thread([this] {
            accept_loop();
        });
        log(MPS_LOG_INFO, 0U, "server", "started", "server is listening");
        return MPS_STATUS_OK;
    }

    bool submit(Command command) noexcept {
        try {
            std::unique_lock lock(command_mutex);
            const auto retained = command.retained_bytes();
            command_space_cv.wait(lock, [this, retained] {
                return broker_stopping.load() ||
                       (commands.size() < config.command_queue_limit &&
                        (commands.empty() ||
                         retained <= config.command_queue_bytes_limit -
                         std::min<std::size_t>(
                             command_bytes,
                             config.command_queue_bytes_limit)));
            });
            if (broker_stopping.load()) {
                return false;
            }
            commands.push_back(std::move(command));
            command_bytes += retained;
            command_cv.notify_one();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool enqueue(core::ConnectionId id, protocol::SharedFrame frame) override {
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock(connections_mutex);
            if (const auto found = connections.find(id); found != connections.end()) {
                connection = found->second;
            }
        }
        return connection && connection->enqueue(std::move(frame));
    }

    void close(core::ConnectionId id, core::CloseReason reason) override {
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock(connections_mutex);
            if (const auto found = connections.find(id); found != connections.end()) {
                connection = found->second;
            }
        }
        if (connection) {
            if (reason == core::CloseReason::queue_limit) {
                log(MPS_LOG_WARNING, id, "transport", "queue_limit",
                    "connection output queue limit exceeded");
            }
            if (reason == core::CloseReason::protocol_error) {
                connection->drain_and_stop();
            } else {
                connection->stop();
            }
        }
    }

    void request_stop() noexcept {
        const auto current = state.load();
        if (current == State::created || current == State::stopped) {
            if (current == State::created) {
                state.store(State::stopped);
            }
            return;
        }
        state.store(State::stopping);
        const int fd = listener.load();
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
        std::vector<std::shared_ptr<Connection>> snapshot;
        {
            std::lock_guard lock(connections_mutex);
            snapshot.reserve(connections.size());
            for (auto& [id, connection] : connections) {
                (void)id;
                snapshot.push_back(connection);
            }
        }
        for (const auto& connection : snapshot) {
            connection->stop_reading();
        }
    }

    void wait() noexcept {
        std::lock_guard wait_lock(wait_mutex);
        request_stop();
        if (accept_thread.joinable()) {
            accept_thread.join();
        }
        const int listener_fd = listener.exchange(-1);
        if (listener_fd >= 0) {
            ::close(listener_fd);
        }
        std::vector<std::shared_ptr<Connection>> snapshot;
        {
            std::lock_guard lock(connections_mutex);
            snapshot.reserve(connections.size());
            for (auto& [id, connection] : connections) {
                (void)id;
                snapshot.push_back(connection);
            }
        }
        for (const auto& connection : snapshot) {
            connection->stop_reading();
            connection->join_reader();
        }
        broker_stopping.store(true);
        command_cv.notify_all();
        command_space_cv.notify_all();
        if (broker_thread.joinable()) {
            broker_thread.join();
        }
        for (const auto& connection : snapshot) {
            connection->drain_output();
        }
        std::mutex deadline_mutex;
        std::condition_variable deadline_cv;
        bool writers_finished = false;
        std::thread deadline([&] {
            std::unique_lock lock(deadline_mutex);
            if (!deadline_cv.wait_for(
                lock, std::chrono::milliseconds(config.shutdown_timeout_ms),
                [&] {
                    return writers_finished;
                })) {
                for (const auto& connection : snapshot) {
                    connection->stop();
                }
            }
        });
        for (const auto& connection : snapshot) {
            connection->join_writer();
        }
        {
            std::lock_guard lock(deadline_mutex);
            writers_finished = true;
        }
        deadline_cv.notify_one();
        deadline.join();
        {
            std::lock_guard lock(connections_mutex);
            connections.clear();
        }
        state.store(State::stopped);
    }

    void log(mps_log_severity_t severity, core::ConnectionId id,
             const char* component, const char* event, const char* message) const
        noexcept {
        if (config.log_callback == nullptr) {
            return;
        }
        const mps_log_record_t record{sizeof(mps_log_record_t), severity, id,
                                      component, event, message};
        try {
            config.log_callback(&record, config.log_user_data);
        } catch (...) {
        }
    }

    void accept_loop() noexcept {
        try {
            while (state.load() == State::running) {
                const int fd = ::accept(listener.load(), nullptr, nullptr);
                if (fd < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if (state.load() != State::running) {
                    ::close(fd);
                    break;
                }
                if (active_connections.load() >= config.max_connections) {
                    ::close(fd);
                    continue;
                }
                const auto id = next_connection_id.fetch_add(1U);
                auto connection = std::make_shared<Connection>(*this, id, fd);
                {
                    std::lock_guard lock(connections_mutex);
                    connections.emplace(id, connection);
                }
                active_connections.fetch_add(1U);
                if (!submit(Command{Command::Kind::opened, id, {}})) {
                    connection->stop();
                    active_connections.fetch_sub(1U);
                    break;
                }
                connection->start();
                log(MPS_LOG_INFO, id, "transport", "connection_accepted",
                    "connection accepted");
            }
        } catch (...) {
            log(MPS_LOG_ERROR, 0U, "transport", "acceptor_exception",
                "acceptor failed");
        }
    }

    void broker_loop() noexcept {
        try {
            for (;;) {
                Command command;
                {
                    std::unique_lock lock(command_mutex);
                    command_cv.wait(lock, [this] {
                        return broker_stopping.load() || !commands.empty();
                    });
                    if (commands.empty() && broker_stopping.load()) {
                        break;
                    }
                    command = std::move(commands.front());
                    commands.pop_front();
                    command_bytes -= command.retained_bytes();
                    command_space_cv.notify_one();
                }
                switch (command.kind) {
                case Command::Kind::opened:
                    broker.connection_opened(command.id);
                    break;
                case Command::Kind::frame:
                    broker.execute(command.id, command.frame);
                    break;
                case Command::Kind::closed:
                    broker.connection_closed(command.id);
                    break;
                }
            }
        } catch (...) {
            log(MPS_LOG_ERROR, 0U, "broker", "dispatcher_exception",
                "broker dispatcher failed");
            request_stop();
        }
    }

    enum class State { created, starting, running, stopping, stopped };

    Config config;
    core::Broker broker;
    std::atomic<State> state{State::created};
    std::atomic<int> listener{-1};
    std::uint16_t bound_port_value{0};
    std::thread accept_thread;
    std::thread broker_thread;
    std::mutex wait_mutex;
    std::atomic<core::ConnectionId> next_connection_id{1U};
    std::atomic<std::uint32_t> active_connections{0U};
    std::mutex connections_mutex;
    std::unordered_map<core::ConnectionId, std::shared_ptr<Connection>> connections;
    std::mutex command_mutex;
    std::condition_variable command_cv;
    std::condition_variable command_space_cv;
    std::deque<Command> commands;
    std::size_t command_bytes{0};
    std::atomic<bool> broker_stopping{false};
};

ServerRuntime::ServerRuntime(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
}

ServerRuntime::~ServerRuntime() = default;

mps_status_t ServerRuntime::start(std::string& error, int& system_code) {
    return impl_->start(error, system_code);
}

std::uint16_t ServerRuntime::bound_port() const noexcept {
    return impl_->bound_port_value;
}

void ServerRuntime::request_stop() noexcept {
    impl_->request_stop();
}

void ServerRuntime::wait() noexcept {
    impl_->wait();
}

} // namespace mps::server