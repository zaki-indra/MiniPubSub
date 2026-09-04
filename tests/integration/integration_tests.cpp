#include "minipubsub/minipubsub.h"

#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
mps_bytes_view_t view(std::string_view value) {
  return {reinterpret_cast<const std::uint8_t *>(value.data()),
          static_cast<std::uint32_t>(value.size())};
}

mps_client_t *client_on(std::uint16_t port) {
  mps_client_config_t *config = nullptr;
  mps_client_t *client = nullptr;
  assert(mps_client_config_create(&config, nullptr) == MPS_STATUS_OK);
  assert(mps_client_config_set_port(config, port, nullptr) == MPS_STATUS_OK);
  assert(mps_client_create(config, &client, nullptr) == MPS_STATUS_OK);
  mps_client_config_destroy(config);
  assert(mps_client_connect(client, nullptr) == MPS_STATUS_OK);
  return client;
}

mps_event_t *poll(mps_client_t *client) {
  mps_event_t *event = nullptr;
  assert(mps_client_poll_event(client, 5000U, &event, nullptr) == MPS_STATUS_OK);
  return event;
}

struct CallbackState {
  std::mutex mutex;
  std::condition_variable cv;
  bool pong{false};
  mps_client_t *client{nullptr};
};

void callback(const mps_event_t *event, void *opaque) {
  auto &state = *static_cast<CallbackState *>(opaque);
  if (mps_event_kind(event) == MPS_EVENT_PONG) {
    std::lock_guard lock(state.mutex);
    state.pong = true;
    state.cv.notify_one();
    assert(mps_client_request_stop(state.client, nullptr) == MPS_STATUS_OK);
  }
}
}  // namespace

int main() {
  mps_server_config_t *server_config = nullptr;
  mps_server_t *server = nullptr;
  assert(mps_server_config_create(&server_config, nullptr) == MPS_STATUS_OK);
  assert(mps_server_config_set_port(server_config, 0U, nullptr) ==
         MPS_STATUS_OK);
  assert(mps_server_create(server_config, &server, nullptr) == MPS_STATUS_OK);
  mps_server_config_destroy(server_config);
  assert(mps_server_start(server, nullptr) == MPS_STATUS_OK);
  std::uint16_t port = 0;
  assert(mps_server_bound_port(server, &port, nullptr) == MPS_STATUS_OK);
  assert(port != 0U);

  auto *subscriber = client_on(port);
  auto *publisher = client_on(port);
  mps_request_id_t subscribe_id = 0;
  const auto channel = view("binary");
  assert(mps_client_subscribe(subscriber, &channel, &subscribe_id,
                              nullptr) == MPS_STATUS_OK);
  auto *event = poll(subscriber);
  assert(mps_event_kind(event) == MPS_EVENT_OK);
  assert(mps_event_request_id(event) == subscribe_id);
  mps_event_destroy(event);

  const std::array<std::uint8_t, 4> bytes{0U, 1U, 2U, 255U};
  mps_request_id_t publish_id = 0;
  const mps_bytes_view_t binary_message{bytes.data(), bytes.size()};
  assert(mps_client_publish(publisher, &channel, &binary_message, &publish_id,
                            nullptr) == MPS_STATUS_OK);
  event = poll(subscriber);
  assert(mps_event_kind(event) == MPS_EVENT_MESSAGE);
  mps_bytes_view_t delivered_channel{};
  mps_bytes_view_t delivered{};
  assert(mps_event_channel(event, &delivered_channel) == MPS_STATUS_OK);
  assert(delivered_channel.length == 6U);
  assert(mps_event_message(event, &delivered) == MPS_STATUS_OK);
  assert(delivered.length == bytes.size());
  assert(std::memcmp(delivered.data, bytes.data(), bytes.size()) == 0);
  mps_event_destroy(event);
  event = poll(publisher);
  assert(mps_event_kind(event) == MPS_EVENT_OK);
  assert(mps_event_request_id(event) == publish_id);
  mps_event_destroy(event);

  constexpr std::size_t thread_count = 4U;
  constexpr std::size_t messages_per_thread = 10U;
  std::vector<std::thread> publishers;
  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    publishers.emplace_back([&, thread_index] {
      for (std::size_t message_index = 0;
           message_index < messages_per_thread; ++message_index) {
        const auto text = std::to_string(thread_index) + ":" +
                          std::to_string(message_index);
        const auto message = view(text);
        mps_request_id_t id = 0;
        assert(mps_client_publish(publisher, &channel, &message, &id, nullptr) ==
               MPS_STATUS_OK);
      }
    });
  }
  for (auto &thread : publishers) {
    thread.join();
  }
  for (std::size_t index = 0; index < thread_count * messages_per_thread;
       ++index) {
    event = poll(subscriber);
    assert(mps_event_kind(event) == MPS_EVENT_MESSAGE);
    mps_event_destroy(event);
  }
  for (std::size_t index = 0; index < thread_count * messages_per_thread;
       ++index) {
    event = poll(publisher);
    assert(mps_event_kind(event) == MPS_EVENT_OK);
    mps_event_destroy(event);
  }

  mps_request_id_t ping_id = 0;
  assert(mps_client_ping(publisher, &ping_id, nullptr) == MPS_STATUS_OK);
  event = poll(publisher);
  assert(mps_event_kind(event) == MPS_EVENT_PONG);
  assert(mps_event_request_id(event) == ping_id);
  mps_event_destroy(event);

  CallbackState callback_state;
  mps_client_config_t *callback_config = nullptr;
  mps_client_t *callback_client = nullptr;
  assert(mps_client_config_create(&callback_config, nullptr) == MPS_STATUS_OK);
  assert(mps_client_config_set_port(callback_config, port, nullptr) ==
         MPS_STATUS_OK);
  assert(mps_client_config_set_event_callback(
             callback_config, callback, &callback_state, nullptr) ==
         MPS_STATUS_OK);
  assert(mps_client_create(callback_config, &callback_client, nullptr) ==
         MPS_STATUS_OK);
  callback_state.client = callback_client;
  mps_client_config_destroy(callback_config);
  assert(mps_client_connect(callback_client, nullptr) == MPS_STATUS_OK);
  event = nullptr;
  assert(mps_client_poll_event(callback_client, 0U, &event, nullptr) ==
         MPS_STATUS_WRONG_EVENT_MODE);
  assert(mps_client_ping(callback_client, &ping_id, nullptr) == MPS_STATUS_OK);
  {
    std::unique_lock lock(callback_state.mutex);
    assert(callback_state.cv.wait_for(lock, std::chrono::seconds(5), [&] {
      return callback_state.pong;
    }));
  }

  mps_client_destroy(callback_client);
  mps_client_destroy(publisher);
  mps_client_destroy(subscriber);
  auto *idle_client = client_on(port);
  assert(mps_server_request_stop(server, nullptr) == MPS_STATUS_OK);
  assert(mps_server_wait(server, nullptr) == MPS_STATUS_OK);
  mps_server_destroy(server);
  event = poll(idle_client);
  assert(mps_event_kind(event) == MPS_EVENT_DISCONNECTED);
  mps_event_destroy(event);
  mps_client_destroy(idle_client);
  return 0;
}
