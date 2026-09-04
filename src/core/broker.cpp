#include "core/broker.hpp"

namespace mps::core
{
Broker::Broker(DeliverySink& sink, std::uint32_t max_channel,
               std::uint32_t max_message)
    : sink_(sink), max_channel_(max_channel), max_message_(max_message) {
}

void Broker::connection_opened(ConnectionId id) {
    reverse_.try_emplace(id);
}

void Broker::send_ok(ConnectionId id, std::uint32_t request_id) {
    if (!sink_.enqueue(id, protocol::encode_frame(
                           static_cast<std::uint8_t>(protocol::Opcode::ok),
                           request_id))) {
        sink_.close(id, CloseReason::queue_limit);
    }
}

void Broker::send_error(ConnectionId id, std::uint32_t request_id,
                        protocol::ErrorCode code, std::string_view message) {
    const auto payload = protocol::encode_error(code, message);
    if (!sink_.enqueue(
        id, protocol::encode_frame(
            static_cast<std::uint8_t>(protocol::Opcode::error),
            request_id, payload))) {
        sink_.close(id, CloseReason::queue_limit);
    }
}

void Broker::execute(ConnectionId id, const protocol::Frame& request) {
    if (!reverse_.contains(id)) {
        return;
    }
    if (request.request_id == 0U || request.flags != 0U) {
        send_error(id, request.request_id, protocol::ErrorCode::invalid_request,
                   "request ID must be non-zero and flags must be zero");
        if (request.flags != 0U) {
            sink_.close(id, CloseReason::protocol_error);
        }
        return;
    }

    const auto opcode = static_cast<protocol::Opcode>(request.opcode);
    protocol::ErrorCode error{};
    std::string channel;
    switch (opcode) {
    case protocol::Opcode::subscribe:
    case protocol::Opcode::unsubscribe: {
        if (!protocol::decode_channel(request.payload, max_channel_, channel,
                                      error)) {
            send_error(id, request.request_id, error, "invalid channel payload");
            return;
        }
        if (opcode == protocol::Opcode::subscribe) {
            channels_[channel].insert(id);
            reverse_[id].insert(channel);
        } else {
            if (auto found = channels_.find(channel); found != channels_.end()) {
                found->second.erase(id);
                if (found->second.empty()) {
                    channels_.erase(found);
                }
            }
            reverse_[id].erase(channel);
        }
        send_ok(id, request.request_id);
        return;
    }
    case protocol::Opcode::publish: {
        protocol::ChannelMessage publication;
        if (!protocol::decode_channel_message(request.payload, max_channel_,
                                              max_message_, publication, error)) {
            send_error(id, request.request_id, error,
                       "invalid channel/message payload");
            return;
        }
        if (const auto subscribers = channels_.find(publication.channel);
            subscribers != channels_.end()) {
            const auto payload = protocol::encode_channel_message(
                publication.channel, publication.message);
            const auto message = protocol::encode_frame(
                static_cast<std::uint8_t>(protocol::Opcode::message), 0U, payload);
            const auto snapshot = subscribers->second;
            for (const auto subscriber : snapshot) {
                if (!sink_.enqueue(subscriber, message)) {
                    sink_.close(subscriber, CloseReason::queue_limit);
                }
            }
        }
        send_ok(id, request.request_id);
        return;
    }
    case protocol::Opcode::ping:
        if (!request.payload.empty()) {
            send_error(id, request.request_id,
                       protocol::ErrorCode::malformed_frame,
                       "PING payload must be empty");
            return;
        }
        if (!sink_.enqueue(id, protocol::encode_frame(
                               static_cast<std::uint8_t>(
                                   protocol::Opcode::pong),
                               request.request_id))) {
            sink_.close(id, CloseReason::queue_limit);
        }
        return;
    case protocol::Opcode::ok:
    case protocol::Opcode::message:
    case protocol::Opcode::error:
    case protocol::Opcode::pong:
        send_error(id, request.request_id, protocol::ErrorCode::invalid_request,
                   "response opcode sent by client");
        sink_.close(id, CloseReason::protocol_error);
        return;
    default:
        send_error(id, request.request_id, protocol::ErrorCode::unknown_opcode,
                   "unknown opcode");
        return;
    }
}

void Broker::connection_closed(ConnectionId id) {
    const auto reverse = reverse_.find(id);
    if (reverse == reverse_.end()) {
        return;
    }
    for (const auto& channel : reverse->second) {
        if (auto found = channels_.find(channel); found != channels_.end()) {
            found->second.erase(id);
            if (found->second.empty()) {
                channels_.erase(found);
            }
        }
    }
    reverse_.erase(reverse);
}
} // namespace mps::core