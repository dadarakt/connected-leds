#include "mesh_node.h"
#include "esp_mesh.h"
#include "mesh_common.h"
#include "mesh_protocol.h"

void node_rx_task(void *arg) {
  mesh_data_t data;
  mesh_addr_t from;
  uint8_t rx_buf[32];
  int flag;

  data.data = rx_buf;

  while (1) {
    data.size = sizeof(rx_buf);
    esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);

    if (data.size == sizeof(rgb_cmd_t)) {
      rgb_cmd_t *cmd = (rgb_cmd_t *)data.data;
      set_led(cmd->r, cmd->g, cmd->b);
    }
  }
}

void mesh_node_start(void) {
  mesh_init(false);
  xTaskCreate(node_rx_task, "node_rx", 4096, NULL, 5, NULL);
}
