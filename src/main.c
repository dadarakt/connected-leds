#include "esp_crc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/projdefs.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "leds.h"
#include "mesh_common.h"
#include "mesh_config.h"
#include "nvs_flash.h"
#include "ptp_sync.h"
#include "role.h"
#include <stdlib.h>
#include <string.h>

#ifndef IS_ROOT
#error "Build must define ROLE_ROOT or ROLE_NODE"
#endif

static const char *TAG = "esp_now_rgb";
static QueueHandle_t s_espnow_queue;
static uint16_t s_espnow_seq[DATA_MAX] = {0, 0};

typedef enum {
  MSG_TYPE_RGB,
  MSG_TYPE_SYNC,
  MSG_TYPE_DELAY_REQ,
  MSQ_TYPE_DELAY_RES,
} msg_type_t;

typedef enum {
  ROOT_STATE_BROADCASTING,
  ROOT_STATE_UNICASTING,
} root_task_state_t;

typedef enum {
  NODE_STATE_UNCONNECTED,
  NODE_STATE_CONNECTED,
} node_task_state_t;

static void espnow_deinit(send_param_t *send_param);

// ESP-NOW callbacks

/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void send_cb(const esp_now_send_info_t *tx_info,
                    esp_now_send_status_t status) {
  espnow_event_t evt;
  event_send_cb_t *send_cb = &evt.info.send_cb;

  if (tx_info == NULL) {
    ESP_LOGE(TAG, "Send cb arg error");
    return;
  }

  evt.id = ESPNOW_SEND_CB;
  memcpy(send_cb->mac_addr, tx_info->des_addr, ESP_NOW_ETH_ALEN);
  send_cb->status = status;
  if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
    ESP_LOGW(TAG, "Send send queue fail");
  }
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int len) {
  espnow_event_t evt;
  event_recv_cb_t *recv_cb = &evt.info.recv_cb;
  uint8_t *mac_addr = recv_info->src_addr;
  uint8_t *des_addr = recv_info->des_addr;

  if (mac_addr == NULL || data == NULL || len <= 0) {
    ESP_LOGE(TAG, "Receive cb arg error");
    return;
  }

  if (IS_BROADCAST_ADDR(des_addr)) {
    /* If added a peer with encryption before, the receive packets may be
     * encrypted as peer-to-peer message or unencrypted over the broadcast
     * channel. Users can check the destination address to distinguish it.
     */
    ESP_LOGD(TAG, "Receive broadcast ESPNOW data");
  } else {
    ESP_LOGD(TAG, "Receive unicast ESPNOW data");
  }

  evt.id = ESPNOW_RECV_CB;
  memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  recv_cb->data = malloc(len);
  if (recv_cb->data == NULL) {
    ESP_LOGE(TAG, "Malloc receive data fail");
    return;
  }
  memcpy(recv_cb->data, data, len);
  recv_cb->data_len = len;
  if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
    ESP_LOGW(TAG, "Send receive queue fail");
    free(recv_cb->data);
  }
}

/* Parse received ESPNOW data. */
int espnow_data_parse(uint8_t *data, uint16_t data_len, uint8_t *state,
                      uint16_t *seq, uint32_t *magic) {
  espnow_data_t *buf = (espnow_data_t *)data;
  uint16_t crc, crc_cal = 0;

  if (data_len < sizeof(espnow_data_t)) {
    ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
    return -1;
  }

  *state = buf->state;
  *seq = buf->seq_num;
  *magic = buf->magic;
  crc = buf->crc;
  buf->crc = 0;
  crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

  if (crc_cal == crc) {
    return buf->type;
  }

  return -1;
}

/* Prepare ESPNOW data to be sent. */
void espnow_data_prepare(send_param_t *send_param) {
  espnow_data_t *buf = (espnow_data_t *)send_param->buffer;
  buf->type =
      IS_BROADCAST_ADDR(send_param->dest_mac) ? DATA_BROADCAST : DATA_UNICAST;
  buf->seq_num = s_espnow_seq[buf->type]++;
  buf->crc = 0;
  buf->magic = send_param->magic;

  // int64_t now_us = esp_timer_get_time();

  send_param->len = sizeof(espnow_data_t);

  buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

static void led_task(void *pvParameter) {
  led_task_state_t state = {
      .mode = LED_MODE_SEARCHING,
      .period_ms = 500,
      .color = {.r = 100, .g = 0, .b = 0},
  };

  int64_t t0_local_us = esp_timer_get_time();
  bool has_sync = false;

  while (1) {
    led_update_t update;
    // check for incoming LED updates, implicit 20ms delay (50fps)
    if (xQueueReceive(led_queue, &update, pdMS_TO_TICKS(20)) == pdTRUE) {

      if (update.set_period)
        state.period_ms = update.period_ms;
      if (update.set_color)
        state.color = update.color;

      if (update.set_sync) {
        // adjust sender t0 into local clock domain
        int64_t now_us = esp_timer_get_time();
        t0_local_us = update.t0_us + (now_us - update.tx_us);
        has_sync = true;
      }
    }

    int64_t now_us = esp_timer_get_time();
    int64_t period_us = (int64_t)state.period_ms * 1000;

    bool led_on;
    if (state.mode == LED_MODE_SEARCHING || !has_sync) {
      // simple blink while not synced
      led_on = ((now_us / 500000) % 2);
    } else {
      int64_t phase_us = (now_us - t0_local_us) % period_us;
      if (phase_us < 0)
        phase_us += period_us;
      led_on = (phase_us < (period_us / 2));
    }

    leds_set_rgb(led_on, state.color);
  }
}

/**
 * Helper to add a peer and update LED to synced state
 */
static bool add_peer_and_sync_led(const uint8_t *mac_addr,
                                  send_param_t *send_param) {
  if (esp_now_is_peer_exist(mac_addr)) {
    return false;
  }

  esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
  if (peer == NULL) {
    ESP_LOGE(TAG, "Malloc peer information fail");
    return false;
  }
  memset(peer, 0, sizeof(esp_now_peer_info_t));
  peer->channel = ESPNOW_CHANNEL;
  peer->ifidx = ESPNOW_WIFI_IF;
  peer->encrypt = false;  // Unencrypted for initial handshake
  memcpy(peer->peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
  ESP_ERROR_CHECK(esp_now_add_peer(peer));
  free(peer);

  led_update_t u = {
      .set_mode = true,
      .mode = LED_MODE_SYNCED,
      .set_color = true,
      .color = (rgb){0, 50, 0},
  };
  update_led(u);

  return true;
}

#if IS_ROOT
/**
 * Root task: broadcasts to discover nodes, then sends unicast
 */
static void espnow_root_task(void *pvParameter) {
  espnow_event_t evt;
  uint8_t recv_state = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;
  int ret;

  send_param_t *send_param = (send_param_t *)pvParameter;
  root_task_state_t state = ROOT_STATE_BROADCASTING;

  // Set yellow LED for root while broadcasting
  led_update_t led = {
      .set_color = true,
      .color = (rgb){100, 100, 0},
  };
  update_led(led);

  vTaskDelay(pdMS_TO_TICKS(3000));
  ESP_LOGI(TAG, "Root: State=BROADCASTING, starting broadcast");

  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Send error");
    espnow_deinit(send_param);
    vTaskDelete(NULL);
  }

  while (xQueueReceive(s_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
    switch (evt.id) {
    case ESPNOW_SEND_CB: {
      event_send_cb_t *send_cb = &evt.info.send_cb;

      ESP_LOGD(TAG, "Root sent to " MACSTR ", status: %d",
               MAC2STR(send_cb->mac_addr), send_cb->status);

      if (send_param->delay > 0) {
        vTaskDelay(pdMS_TO_TICKS(send_param->delay));
      }

      espnow_data_prepare(send_param);

      if (state == ROOT_STATE_BROADCASTING) {
        // Continue broadcasting to discover nodes
        if (esp_now_send(send_param->dest_mac, send_param->buffer,
                         send_param->len) != ESP_OK) {
          ESP_LOGE(TAG, "Send error");
          espnow_deinit(send_param);
          vTaskDelete(NULL);
        }
      } else if (state == ROOT_STATE_UNICASTING) {
        // Send unicast to connected node
        if (esp_now_send(send_param->dest_mac, send_param->buffer,
                         send_param->len) != ESP_OK) {
          ESP_LOGE(TAG, "Send error");
          espnow_deinit(send_param);
          vTaskDelete(NULL);
        }
      }
      break;
    }
    case ESPNOW_RECV_CB: {
      event_recv_cb_t *recv_cb = &evt.info.recv_cb;

      ret = espnow_data_parse(recv_cb->data, recv_cb->data_len, &recv_state,
                              &recv_seq, &recv_magic);
      free(recv_cb->data);

      if (ret == DATA_BROADCAST || ret == DATA_UNICAST) {
        ESP_LOGI(TAG, "Root: Received %s from node " MACSTR,
                 ret == DATA_BROADCAST ? "broadcast" : "unicast",
                 MAC2STR(recv_cb->mac_addr));

        if (add_peer_and_sync_led(recv_cb->mac_addr, send_param)) {
          // Switch to unicast to this node
          memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
          send_param->broadcast = false;
          send_param->unicast = true;

          // Transition to UNICASTING state
          state = ROOT_STATE_UNICASTING;
          ESP_LOGI(TAG, "Root: State=UNICASTING, connected to " MACSTR,
                   MAC2STR(recv_cb->mac_addr));
        }
      }
      break;
    }
    default:
      ESP_LOGE(TAG, "Callback type error: %d", evt.id);
      break;
    }
  }
}

#else
/**
 * Node task: listens for root broadcasts, responds, receives unicast
 */
static void espnow_node_task(void *pvParameter) {
  espnow_event_t evt;
  uint8_t recv_state = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;
  int ret;

  send_param_t *send_param = (send_param_t *)pvParameter;
  node_task_state_t state = NODE_STATE_UNCONNECTED;

  ESP_LOGI(TAG, "Node: State=UNCONNECTED, waiting for root broadcasts");

  while (xQueueReceive(s_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
    switch (evt.id) {
    case ESPNOW_SEND_CB: {
      event_send_cb_t *send_cb = &evt.info.send_cb;
      ESP_LOGD(TAG, "Node sent to " MACSTR ", status: %d",
               MAC2STR(send_cb->mac_addr), send_cb->status);
      break;
    }
    case ESPNOW_RECV_CB: {
      event_recv_cb_t *recv_cb = &evt.info.recv_cb;

      ret = espnow_data_parse(recv_cb->data, recv_cb->data_len, &recv_state,
                              &recv_seq, &recv_magic);
      free(recv_cb->data);

      if (ret == DATA_BROADCAST) {
        ESP_LOGI(TAG, "Node: Received broadcast from root " MACSTR,
                 MAC2STR(recv_cb->mac_addr));

        if (add_peer_and_sync_led(recv_cb->mac_addr, send_param)) {
          // Send acknowledgment back to root
          memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
          espnow_data_prepare(send_param);
          esp_now_send(send_param->dest_mac, send_param->buffer,
                       send_param->len);
          ESP_LOGI(TAG, "Node: Sent ack to root " MACSTR,
                   MAC2STR(recv_cb->mac_addr));

          // Transition to CONNECTED state with yellow LED
          state = NODE_STATE_CONNECTED;
          led_update_t u = {
              .set_color = true,
              .color = (rgb){100, 100, 0},
          };
          update_led(u);
          ESP_LOGI(TAG, "Node: State=CONNECTED");
        }
      } else if (ret == DATA_UNICAST) {
        ESP_LOGI(TAG, "Node: Received unicast seq=%d from root", recv_seq);
        // Future: handle sync messages here
      }
      break;
    }
    default:
      ESP_LOGE(TAG, "Callback type error: %d", evt.id);
      break;
    }
  }
}
#endif

static esp_err_t espnow_init(void) {
  send_param_t *send_param;

  s_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
  if (s_espnow_queue == NULL) {
    ESP_LOGE(TAG, "Create queue fail");
    return ESP_FAIL;
  }

  /* Initialize ESPNOW and register sending and receiving callback function.
   */
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
  ESP_ERROR_CHECK(esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW));
  ESP_ERROR_CHECK(esp_wifi_connectionless_module_set_wake_interval(
      CONFIG_ESPNOW_WAKE_INTERVAL));
#endif
  /* Set primary master key. */
  ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)ESPNOW_PMK));

  /* Add broadcast peer information to peer list. */
  esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
  if (peer == NULL) {
    ESP_LOGE(TAG, "Malloc peer information fail");
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
    esp_now_deinit();
    return ESP_FAIL;
  }
  memset(peer, 0, sizeof(esp_now_peer_info_t));
  peer->channel = ESPNOW_CHANNEL;
  peer->ifidx = ESPNOW_WIFI_IF;
  peer->encrypt = false;
  memcpy(peer->peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
  ESP_ERROR_CHECK(esp_now_add_peer(peer));
  free(peer);

  /* Initialize sending parameters. */
  send_param = malloc(sizeof(send_param_t));
  if (send_param == NULL) {
    ESP_LOGE(TAG, "Malloc send parameter fail");
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
    esp_now_deinit();
    return ESP_FAIL;
  }
  memset(send_param, 0, sizeof(send_param_t));
  send_param->delay = ESPNOW_SEND_DELAY;
  send_param->len = ESPNOW_SEND_LEN;
  send_param->buffer = malloc(ESPNOW_SEND_LEN);
  if (send_param->buffer == NULL) {
    ESP_LOGE(TAG, "Malloc send buffer fail");
    free(send_param);
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
    esp_now_deinit();
    return ESP_FAIL;
  }

#if IS_ROOT
  ESP_LOGI(TAG, "Initializing as ROOT");
  send_param->unicast = false;
  send_param->broadcast = true;
  send_param->magic = UINT32_MAX;
  memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
  espnow_data_prepare(send_param);
  xTaskCreate(espnow_root_task, "espnow_root", 2048, send_param, 4, NULL);
#else
  ESP_LOGI(TAG, "Initializing as NODE");
  send_param->unicast = false;
  send_param->broadcast = false;
  send_param->magic = 0;
  memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
  espnow_data_prepare(send_param);
  xTaskCreate(espnow_node_task, "espnow_node", 2048, send_param, 4, NULL);
#endif

  return ESP_OK;
}

// Initialize Wi-Fi STA mode
void wifi_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if ESPNOW_ENABLE_LONG_RANGE
  ESP_ERROR_CHECK(esp_wifi_set_protocol(
      ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                          WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));
#endif
}

static void espnow_deinit(send_param_t *send_param) {
  free(send_param->buffer);
  free(send_param);
  vQueueDelete(s_espnow_queue);
  s_espnow_queue = NULL;
  esp_now_deinit();
}

void app_main(void) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  leds_init();
  xTaskCreate(led_task, "led_task", 2048, NULL, 1, NULL);

  wifi_init();
  espnow_init();
}
