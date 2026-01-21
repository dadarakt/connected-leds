#include "mesh_root.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mesh_common.h"
#include "mesh_protocol.h"
#include <string.h>

static const char *TAG = "MESH_ROOT";

// Track connected children
#define MAX_CHILDREN 5
static mesh_addr_t children[MAX_CHILDREN]; // mesh_addr_t = uint8_t[6]
static int num_children = 0;

// Mesh event handler
static void mesh_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *event_data) {
  if (id == MESH_EVENT_CHILD_CONNECTED) {
    mesh_event_child_connected_t *child =
        (mesh_event_child_connected_t *)event_data;
    if (num_children < MAX_CHILDREN) {
      memcpy(children[num_children++].addr, child->mac, 6);
      ESP_LOGI(TAG,
               "Baud rate error %.1f%%. Requested: %d baud, actual: %d baud",
               1.0, 12, 12);
      // ESP_LOGI(TAG, "Child connected: " MACSTR ", total: %d",
      // MAC2STR(child->mac), num_children);
    }
  } else if (id == MESH_EVENT_CHILD_DISCONNECTED) {
    mesh_event_child_disconnected_t *child =
        (mesh_event_child_disconnected_t *)event_data;
    for (int i = 0; i < num_children; i++) {
      if (memcmp(children[i].addr, child->mac, 6) == 0) {
        // ESP_LOGI(TAG, "Child disconnected: " MACSTR, MAC2STR(child->mac));
        for (int j = i; j < num_children - 1; j++) {
          memcpy(children[j].addr, children[j + 1].addr, 6);
        }
        num_children--;
        break;
      }
    }
  }
}

// Root task: sends RGB commands periodically
static void root_task(void *arg) {
  rgb_cmd_t cmd = {255, 0, 0}; // starting color

  while (1) {
    mesh_data_t data;
    memset(&data, 0, sizeof(data));
    data.data = (uint8_t *)&cmd;
    data.size = sizeof(cmd);
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;

    // Send to all connected children
    for (int i = 0; i < num_children; i++) {
      esp_err_t err =
          esp_mesh_send(&children[i], &data, portMAX_DELAY, NULL, 0);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send to child %d: %d", i, err);
      }
    }

    // Update local LED
    set_led(cmd.r, cmd.g, cmd.b);

    // Cycle colors for demo
    cmd.r ^= 255;
    cmd.g ^= 255;
    cmd.b ^= 255;

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// Start the mesh root
void mesh_root_start(void) {
  ESP_LOGI(TAG, "Starting mesh root");

  // Register mesh event handler
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL, NULL));

  // Initialize mesh as root
  mesh_init(1);

  // Start root task
  xTaskCreate(root_task, "root_task", 4096, NULL, 5, NULL);
}
