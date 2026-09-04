#include "minipubsub/minipubsub.h"

#include <assert.h>
#include <string.h>

int main(void) {
  assert(mps_abi_version() == MPS_ABI_VERSION);
  assert(mps_protocol_version() == MPS_PROTOCOL_VERSION);
  assert(strcmp(mps_version_string(), "1.0.0") == 0);
  mps_client_config_t *config = NULL;
  assert(mps_client_config_create(&config, NULL) == MPS_STATUS_OK);
  assert(config != NULL);
  mps_client_config_destroy(config);
  return 0;
}
