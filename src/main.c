#include "leds.h"
#include "mesh_common.h"
#include "role.h"

#ifndef IS_ROOT
#error "Build must define ROLE_ROOT or ROLE_NODE"
#endif

void app_main(void) {
  leds_init();
  leds_start();

  mesh_init(IS_ROOT);
}
