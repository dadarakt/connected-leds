#include "mesh_node.h"
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

static const char *TAG = "mesh_node";

typedef enum {
  NODE_STATE_UNCONNECTED,
  NODE_STATE_CONNECTED,
} node_conn_state_t;

typedef struct {
  send_param_t *send_param;
  QueueHandle_t queue;
} node_task_params_t;

// --- Time sync state and helpers ---

typedef struct {
  int64_t pending_t1;
  int64_t pending_t2;
  int64_t pending_t3;
  uint32_t pending_sync_id;
  bool synced;
} sync_state_t;

static void sync_handle_sync_msg(sync_state_t *ss, send_param_t *send_param,
                                 uint8_t *data) {
  int64_t t2 = esp_timer_get_time();
  espnow_data_t *buf = (espnow_data_t *)data;
  sync_msg_t *sync = (sync_msg_t *)buf->payload;

  ss->pending_t1 = sync->t_1;
  ss->pending_t2 = t2;
  ss->pending_sync_id = sync->sync_id;

  ESP_LOGI(TAG, "Received sync id=%lu t1=%lld t2=%lld",
           (unsigned long)ss->pending_sync_id, ss->pending_t1, ss->pending_t2);

  delay_request_t dreq = {.sync_id = ss->pending_sync_id};
  ss->pending_t3 = esp_timer_get_time();
  espnow_data_prepare_payload(send_param, PAYLOAD_TYPE_DELAY_REQ, &dreq,
                              sizeof(dreq));
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Delay request send error");
  }
}

// Returns true if the response was valid and sync was applied.
static bool sync_handle_delay_resp(sync_state_t *ss, uint8_t *data) {
  int64_t t5 = esp_timer_get_time();
  espnow_data_t *buf = (espnow_data_t *)data;
  delay_response_t *dresp = (delay_response_t *)buf->payload;

  if ((uint32_t)dresp->sync_id != ss->pending_sync_id) {
    ESP_LOGW(TAG, "sync_id mismatch: got %ld expected %lu",
             (long)dresp->sync_id, (unsigned long)ss->pending_sync_id);
    return false;
  }

  int64_t rtt = t5 - ss->pending_t3;
  int64_t one_way = rtt / 2;
  int64_t clock_offset = (ss->pending_t1 + one_way) - ss->pending_t2;

  ESP_LOGI(TAG, "Sync id=%lu rtt=%lld us one_way=%lld us offset=%lld us",
           (unsigned long)ss->pending_sync_id, rtt, one_way, clock_offset);

  led_update_t u = {
      .set_sync = true,
      .t0_us = ss->pending_t2 - ss->pending_t1 - one_way,
      .tx_us = t5,
  };
  update_led(u);

  if (!ss->synced) {
    ss->synced = true;
    led_update_t mode_u = {
        .set_mode = true,
        .mode = LED_MODE_SYNCED,
    };
    update_led(mode_u);
  }

  return true;
}

// --- Connection handling ---

// Returns true if the node successfully connected to the root.
static bool handle_root_broadcast(send_param_t *send_param,
                                  const uint8_t *mac_addr) {
  if (!esp_now_is_peer_exist(mac_addr)) {
    if (!espnow_add_peer(mac_addr)) {
      return false;
    }
  }

  memcpy(send_param->dest_mac, mac_addr, ESP_NOW_ETH_ALEN);
  espnow_data_prepare(send_param);
  esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len);
  ESP_LOGI(TAG, "Sent ack to root " MACSTR, MAC2STR(mac_addr));

  led_update_t u = {
      .set_color = true,
      .color = (rgb){100, 100, 0},
  };
  update_led(u);
  ESP_LOGI(TAG, "State=CONNECTED");
  return true;
}

// --- Node task ---

static void node_task(void *pvParameter) {
  node_task_params_t *params = (node_task_params_t *)pvParameter;
  send_param_t *send_param = params->send_param;
  QueueHandle_t queue = params->queue;
  free(params);

  espnow_event_t evt;
  uint8_t recv_payload_type = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;

  node_conn_state_t state = NODE_STATE_UNCONNECTED;
  sync_state_t ss = {0};
  TickType_t last_sync_tick = 0;

  ESP_LOGI(TAG, "State=UNCONNECTED, waiting for root broadcasts");

  while (true) {
    TickType_t wait_ticks;
    if (state == NODE_STATE_CONNECTED) {
      TickType_t now = xTaskGetTickCount();
      TickType_t elapsed = now - last_sync_tick;
      TickType_t timeout = pdMS_TO_TICKS(ESPNOW_NODE_TIMEOUT_MS);
      if (elapsed >= timeout) {
        wait_ticks = 0;
      } else {
        wait_ticks = timeout - elapsed;
      }
    } else {
      wait_ticks = portMAX_DELAY;
    }

    BaseType_t got = xQueueReceive(queue, &evt, wait_ticks);

    if (got != pdTRUE) {
      // Timeout: lost connection to root
      ESP_LOGW(TAG, "Sync timeout, resetting to UNCONNECTED");
      state = NODE_STATE_UNCONNECTED;
      memset(&ss, 0, sizeof(ss));
      led_update_t u = {
          .set_mode = true,
          .mode = LED_MODE_SEARCHING,
      };
      update_led(u);
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

      if (state == NODE_STATE_UNCONNECTED && ret == DATA_BROADCAST) {
        ESP_LOGI(TAG, "Received broadcast from root " MACSTR,
                 MAC2STR(recv_cb->mac_addr));
        if (handle_root_broadcast(send_param, recv_cb->mac_addr)) {
          state = NODE_STATE_CONNECTED;
          last_sync_tick = xTaskGetTickCount();
        }
      } else if (state == NODE_STATE_CONNECTED &&
                 recv_payload_type == PAYLOAD_TYPE_SYNC) {
        last_sync_tick = xTaskGetTickCount();
        sync_handle_sync_msg(&ss, send_param, recv_cb->data);
      } else if (state == NODE_STATE_CONNECTED &&
                 recv_payload_type == PAYLOAD_TYPE_DELAY_RESP) {
        sync_handle_delay_resp(&ss, recv_cb->data);
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

void mesh_node_start(send_param_t *send_param, QueueHandle_t queue) {
  ESP_LOGI(TAG, "Starting node");

  node_task_params_t *params = malloc(sizeof(node_task_params_t));
  params->send_param = send_param;
  params->queue = queue;

  xTaskCreate(node_task, "mesh_node", 4096, params, 4, NULL);
}
