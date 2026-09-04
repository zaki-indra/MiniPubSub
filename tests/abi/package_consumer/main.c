#include <minipubsub/minipubsub.h>

int main(void) {
  return mps_abi_version() == MPS_ABI_VERSION ? 0 : 1;
}
