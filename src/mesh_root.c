#include "mesh_root.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "mesh_common.h"
#include "mesh_config.h"
#include "ptp_sync.h"
#include <string.h>

static const char *TAG = "mesh_root";

typedef struct {
  send_param_t *send_param;
  QueueHandle_t queue;
} root_task_params_t;

// --- Time sync helpers ---

static void sync_send(send_param_t *send_param, uint32_t *sync_id) {
  memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
  sync_msg_t sync = {.t_1 = esp_timer_get_time(), .sync_id = (*sync_id)++};
  espnow_data_prepare_payload(send_param, PAYLOAD_TYPE_SYNC, &sync,
                              sizeof(sync));
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Sync send error");
  }
  ESP_LOGI(TAG, "Sent sync id=%lu t1=%lld", (unsigned long)sync.sync_id,
           sync.t_1);
}

static void sync_handle_delay_req(send_param_t *send_param, uint8_t *data,
                                  const uint8_t *node_mac) {
  espnow_data_t *buf = (espnow_data_t *)data;
  delay_request_t *dreq = (delay_request_t *)buf->payload;
  uint32_t req_sync_id = dreq->sync_id;

  // Temporarily switch to unicast for this node
  memcpy(send_param->dest_mac, node_mac, ESP_NOW_ETH_ALEN);

  delay_response_t dresp = {.t_4 = esp_timer_get_time(),
                            .sync_id = req_sync_id};
  espnow_data_prepare_payload(send_param, PAYLOAD_TYPE_DELAY_RESP, &dresp,
                              sizeof(dresp));
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Delay response send error");
  }
  ESP_LOGI(TAG, "Sent delay_response id=%lu t4=%lld",
           (unsigned long)req_sync_id, dresp.t_4);

  // Restore broadcast MAC
  memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
}

// --- Connection handling ---

// Returns true if a new node was added.
static bool handle_node_response(const uint8_t *mac_addr) {
  return espnow_add_peer(mac_addr);
}

// --- Root task ---

static void root_task(void *pvParameter) {
  root_task_params_t *params = (root_task_params_t *)pvParameter;
  send_param_t *send_param = params->send_param;
  QueueHandle_t queue = params->queue;
  free(params);

  espnow_event_t evt;
  uint8_t recv_payload_type = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;

  uint32_t sync_id = 0;
  bool has_peers = false;
  TickType_t last_sync_tick = 0;

  // Set yellow LED for root while broadcasting
  led_update_t led = {
      .set_color = true,
      .color = (rgb){100, 100, 0},
  };
  update_led(led);

  vTaskDelay(pdMS_TO_TICKS(3000));
  ESP_LOGI(TAG, "Starting broadcast loop");

  // Send initial discovery broadcast
  espnow_data_prepare(send_param);
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Send error");
    espnow_deinit(send_param, queue);
    vTaskDelete(NULL);
  }

  TickType_t wait_ticks = pdMS_TO_TICKS(ESPNOW_DISCOVERY_INTERVAL_MS);

  while (true) {
    BaseType_t got = xQueueReceive(queue, &evt, wait_ticks);

    if (got != pdTRUE) {
      // Timeout: check if it's time for a sync or discovery
      TickType_t now = xTaskGetTickCount();
      if (has_peers &&
          (now - last_sync_tick) >= pdMS_TO_TICKS(ESPNOW_SYNC_INTERVAL_MS)) {
        sync_send(send_param, &sync_id);
        last_sync_tick = now;
      } else {
        // Send discovery broadcast
        memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
        espnow_data_prepare(send_param);
        if (esp_now_send(send_param->dest_mac, send_param->buffer,
                         send_param->len) != ESP_OK) {
          ESP_LOGE(TAG, "Discovery send error");
        }
        ESP_LOGD(TAG, "Sent discovery broadcast");
      }
      continue;
    }

    switch (evt.id) {
    case ESPNOW_SEND_CB: {
      event_send_cb_t *send_cb = &evt.info.send_cb;
      ESP_LOGD(TAG, "Sent to " MACSTR ", status: %d",
               MAC2STR(send_cb->mac_addr), send_cb->status);
      break;
    }
    case ESPNOW_RECV_CB: {
      event_recv_cb_t *recv_cb = &evt.info.recv_cb;

      int ret = espnow_data_parse(recv_cb->data, recv_cb->data_len,
                                  &recv_payload_type, &recv_seq, &recv_magic);
      if (ret < 0) {
        free(recv_cb->data);
        break;
      }

      if (recv_payload_type == PAYLOAD_TYPE_DELAY_REQ) {
        sync_handle_delay_req(send_param, recv_cb->data, recv_cb->mac_addr);
      } else {
        // Bare header = node ack
        ESP_LOGI(TAG, "Received ack from node " MACSTR,
                 MAC2STR(recv_cb->mac_addr));
        if (handle_node_response(recv_cb->mac_addr)) {
          ESP_LOGI(TAG, "New peer added: " MACSTR, MAC2STR(recv_cb->mac_addr));
        }
        if (!has_peers) {
          has_peers = true;
          last_sync_tick =
              xTaskGetTickCount() - pdMS_TO_TICKS(ESPNOW_SYNC_INTERVAL_MS);
          int64_t now = esp_timer_get_time();
          led_update_t u = {
              .set_mode = true,
              .mode = LED_MODE_SYNCED,
              .set_sync = true,
              .t0_us = 0,
              .tx_us = now,
          };
          update_led(u);
        }
      }

      free(recv_cb->data);
      break;
    }
    default:
      ESP_LOGE(TAG, "Callback type error: %d", evt.id);
      break;
    }
  }
}

void mesh_root_start(send_param_t *send_param, QueueHandle_t queue) {
  ESP_LOGI(TAG, "Starting root");

  root_task_params_t *params = malloc(sizeof(root_task_params_t));
  params->send_param = send_param;
  params->queue = queue;

  xTaskCreate(root_task, "mesh_root", 4096, params, 4, NULL);
}
