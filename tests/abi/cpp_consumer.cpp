#include "minipubsub/minipubsub.h"

#include <cassert>
#include <cstdint>
#include <type_traits>

int main() {
  static_assert(std::is_same_v<decltype(mps_abi_version()), std::uint32_t>);
  assert(mps_abi_version() == MPS_ABI_VERSION);
  mps_server_config_t *config = nullptr;
  assert(mps_server_config_create(&config, nullptr) == MPS_STATUS_OK);
  mps_server_config_destroy(config);
  return 0;
}
