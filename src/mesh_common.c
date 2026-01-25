#include "mesh_common.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "leds.h"
#include "mesh_config.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "MESH_COMMON";

// list implementation
static struct Node *followers = NULL;

void add_follower(uint8_t *mac_addr) {
  struct Node *new_follower = NULL;
  new_follower = (struct Node *)malloc(sizeof(struct Node));
  if (!new_follower) {
    ESP_LOGE(TAG, "Malloc follower fail");
  }
  memcpy(new_follower->mac_addr, &mac_addr, ESP_NOW_ETH_ALEN);
  new_follower->last_ping = esp_timer_get_time();

  struct Node *temp = followers;
  if (temp == NULL) {
    followers = new_follower;
    return;
  }

  // find last node
  while (temp->next != NULL) {
    temp = temp->next;
  }

  temp->next = new_follower;
}

struct Node *get_follower(uint8_t *mac_addr) {
  struct Node *temp = followers;
  while (temp != NULL && !memcmp(mac_addr, temp->mac_addr, ESP_NOW_ETH_ALEN)) {
    temp = temp->next;
  }

  return temp;
}

void delete_follower(uint8_t *mac_addr) {
  struct Node *before = NULL;
  struct Node *temp = followers;

  while (temp != NULL && !memcmp(mac_addr, temp->mac_addr, ESP_NOW_ETH_ALEN)) {
    before = temp;
    temp = temp->next;
  }

  if (temp == NULL)
    return;

  // delete the only item in list
  if (before == NULL) {
    followers = NULL;
    free(temp->mac_addr);
    free(temp);
    temp = NULL;
    return;
  }

  // there are more items
  before->next = temp->next;
  free(temp->mac_addr);
  free(temp);
  temp = NULL;
  return;
}

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
  // Dummy external router (no internet for you)
  strcpy((char *)cfg.router.ssid, "mesh_dummy");
  cfg.router.ssid_len = strlen("mesh_dummy");
  cfg.router.password[0] = '\0'; // open network

  // initialize mesh
  ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, false));
  ESP_ERROR_CHECK(esp_mesh_init());
  ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
  ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYER));
  // ESP_ERROR_CHECK(esp_mesh_set_type(is_root ? MESH_ROOT : MESH_NODE));
  if (is_root) {
    ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
  }

  ESP_ERROR_CHECK(esp_mesh_start());

  ESP_LOGI(TAG, "Mesh initialization complete");
}
