#include "mesh_common.h"
#include "role.h"

// Forward declare both role functions for linter
void mesh_root_start(void);
void mesh_node_start(void);

void app_main(void) {
  mesh_common_init(); // common init: Wi-Fi + LEDs + NVS

#if IS_ROOT
  mesh_root_start();
#else
  mesh_node_start();
#endif
}
