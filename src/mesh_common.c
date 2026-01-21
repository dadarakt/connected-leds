#include "mesh_common.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_wifi.h"
#include "mesh_config.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "MESH_COMMON";

void mesh_common_init(void) {
  // Initialize non-volatile storage (required by ESP32 Wi-Fi)
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Initialize network interface
  ESP_ERROR_CHECK(esp_netif_init());

  // Create default event loop
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Initialize Wi-Fi
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
  ESP_ERROR_CHECK(esp_wifi_start());

  // Configure LED pins
  gpio_set_direction(LED_GPIO_R, GPIO_MODE_OUTPUT);
  gpio_set_direction(LED_GPIO_G, GPIO_MODE_OUTPUT);
  gpio_set_direction(LED_GPIO_B, GPIO_MODE_OUTPUT);

  ESP_LOGI(TAG, "Common initialization done");
}

void set_led(uint8_t r, uint8_t g, uint8_t b) {
  gpio_set_level(LED_GPIO_R, r > 0);
  gpio_set_level(LED_GPIO_G, g > 0);
  gpio_set_level(LED_GPIO_B, b > 0);
}

void mesh_init(int is_root) {
  ESP_LOGI(TAG, "Initializing mesh as %s", is_root ? "ROOT" : "NODE");

  mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();

  // mesh_id must be set
  cfg.mesh_id = (mesh_addr_t){{0x32, 0x44, 0x11, 0xAA, 0xBB, 0xCC}};

  // optional parameters
  cfg.channel = MESH_CHANNEL;
  cfg.mesh_ap.max_connection = 2;

  // initialize mesh
  ESP_ERROR_CHECK(esp_mesh_init());
  ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
  ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYER));
  ESP_ERROR_CHECK(esp_mesh_set_type(is_root ? MESH_ROOT : MESH_NODE));
  ESP_ERROR_CHECK(esp_mesh_start());

  ESP_LOGI(TAG, "Mesh initialization complete");
}
