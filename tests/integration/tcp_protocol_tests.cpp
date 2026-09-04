#include "minipubsub/minipubsub.h"
#include "protocol/codec.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <span>
#include <vector>

namespace {
int connect_to(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
  assert(::connect(fd, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0);
  return fd;
}

void send_fragmented(int fd, std::span<const std::uint8_t> bytes) {
  for (const auto byte : bytes) {
    ssize_t result;
    do {
      result = ::send(fd, &byte, 1U, MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);
    assert(result == 1);
  }
}

std::vector<std::uint8_t> receive_frame(int fd) {
  std::array<std::uint8_t, 16> header{};
  std::size_t offset = 0;
  while (offset < header.size()) {
    const auto count = ::recv(fd, header.data() + offset, header.size() - offset, 0);
    assert(count > 0);
    offset += static_cast<std::size_t>(count);
  }
  const auto payload_size =
      (static_cast<std::uint32_t>(header[12]) << 24U) |
      (static_cast<std::uint32_t>(header[13]) << 16U) |
      (static_cast<std::uint32_t>(header[14]) << 8U) |
      static_cast<std::uint32_t>(header[15]);
  std::vector<std::uint8_t> frame(header.begin(), header.end());
  frame.resize(header.size() + payload_size);
  while (offset < frame.size()) {
    const auto count = ::recv(fd, frame.data() + offset, frame.size() - offset, 0);
    assert(count > 0);
    offset += static_cast<std::size_t>(count);
  }
  return frame;
}
}  // namespace

int main() {
  mps_server_config_t *config = nullptr;
  mps_server_t *server = nullptr;
  assert(mps_server_config_create(&config, nullptr) == MPS_STATUS_OK);
  assert(mps_server_config_set_port(config, 0U, nullptr) == MPS_STATUS_OK);
  assert(mps_server_create(config, &server, nullptr) == MPS_STATUS_OK);
  mps_server_config_destroy(config);
  assert(mps_server_start(server, nullptr) == MPS_STATUS_OK);
  std::uint16_t port = 0;
  assert(mps_server_bound_port(server, &port, nullptr) == MPS_STATUS_OK);

  int fd = connect_to(port);
  const auto unknown = mps::protocol::encode_frame(0x55U, 7U);
  send_fragmented(fd, *unknown);
  auto response = receive_frame(fd);
  assert(response[5] == static_cast<std::uint8_t>(mps::protocol::Opcode::error));
  assert(response[11] == 7U);
  assert(response[16] == 0U && response[17] == 1U);

  const auto ping = mps::protocol::encode_frame(
      static_cast<std::uint8_t>(mps::protocol::Opcode::ping), 8U);
  send_fragmented(fd, *ping);
  response = receive_frame(fd);
  assert(response[5] == static_cast<std::uint8_t>(mps::protocol::Opcode::pong));
  assert(response[11] == 8U);

  auto invalid_flags = *ping;
  invalid_flags[7] = 1U;
  send_fragmented(fd, invalid_flags);
  response = receive_frame(fd);
  assert(response[5] == static_cast<std::uint8_t>(mps::protocol::Opcode::error));
  assert(response[16] == 0U && response[17] == 6U);
  std::uint8_t byte = 0;
  assert(::recv(fd, &byte, 1U, 0) == 0);
  ::close(fd);

  fd = connect_to(port);
  auto bad_magic = *ping;
  bad_magic[0] = 0U;
  send_fragmented(fd, bad_magic);
  assert(::recv(fd, &byte, 1U, 0) == 0);
  ::close(fd);

  mps_server_destroy(server);
  return 0;
}
