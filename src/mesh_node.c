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

static void node_task(void *pvParameter) {
  node_task_params_t *params = (node_task_params_t *)pvParameter;
  send_param_t *send_param = params->send_param;
  QueueHandle_t queue = params->queue;
  free(params);

  espnow_event_t evt;
  uint8_t recv_payload_type = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;
  int ret;

  node_conn_state_t state = NODE_STATE_UNCONNECTED;
  bool synced = false;

  // Pending sync state
  int64_t pending_t1 = 0, pending_t2 = 0, pending_t3 = 0;
  uint32_t pending_sync_id = 0;

  ESP_LOGI(TAG, "State=UNCONNECTED, waiting for root broadcasts");

  while (xQueueReceive(queue, &evt, portMAX_DELAY) == pdTRUE) {
    switch (evt.id) {
    case ESPNOW_SEND_CB: {
      event_send_cb_t *send_cb = &evt.info.send_cb;
      ESP_LOGD(TAG, "Sent to " MACSTR ", status: %d",
               MAC2STR(send_cb->mac_addr), send_cb->status);
      break;
    }
    case ESPNOW_RECV_CB: {
      event_recv_cb_t *recv_cb = &evt.info.recv_cb;

      ret = espnow_data_parse(recv_cb->data, recv_cb->data_len,
                              &recv_payload_type, &recv_seq, &recv_magic);

      if (ret < 0) {
        free(recv_cb->data);
        break;
      }

      if (state == NODE_STATE_UNCONNECTED && ret == DATA_BROADCAST) {
        ESP_LOGI(TAG, "Received broadcast from root " MACSTR,
                 MAC2STR(recv_cb->mac_addr));

        if (espnow_add_peer(recv_cb->mac_addr)) {
          memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
          espnow_data_prepare(send_param);
          esp_now_send(send_param->dest_mac, send_param->buffer,
                       send_param->len);
          ESP_LOGI(TAG, "Sent ack to root " MACSTR, MAC2STR(recv_cb->mac_addr));

          state = NODE_STATE_CONNECTED;
          led_update_t u = {
              .set_color = true,
              .color = (rgb){100, 100, 0},
          };
          update_led(u);
          ESP_LOGI(TAG, "State=CONNECTED");
        }
      } else if (state == NODE_STATE_CONNECTED &&
                 recv_payload_type == PAYLOAD_TYPE_SYNC) {
        int64_t t2 = esp_timer_get_time();
        espnow_data_t *buf = (espnow_data_t *)recv_cb->data;
        sync_msg_t *sync = (sync_msg_t *)buf->payload;
        pending_t1 = sync->t_1;
        pending_t2 = t2;
        pending_sync_id = sync->sync_id;

        ESP_LOGI(TAG, "Received sync id=%lu t1=%lld t2=%lld",
                 (unsigned long)pending_sync_id, pending_t1, pending_t2);

        // Immediately send delay_request
        delay_request_t dreq = {.sync_id = pending_sync_id};
        pending_t3 = esp_timer_get_time();
        espnow_data_prepare_payload(send_param, PAYLOAD_TYPE_DELAY_REQ, &dreq,
                                    sizeof(dreq));
        if (esp_now_send(send_param->dest_mac, send_param->buffer,
                         send_param->len) != ESP_OK) {
          ESP_LOGE(TAG, "Delay request send error");
        }
      } else if (state == NODE_STATE_CONNECTED &&
                 recv_payload_type == PAYLOAD_TYPE_DELAY_RESP) {
        int64_t t5 = esp_timer_get_time();
        espnow_data_t *buf = (espnow_data_t *)recv_cb->data;
        delay_response_t *dresp = (delay_response_t *)buf->payload;

        if ((uint32_t)dresp->sync_id != pending_sync_id) {
          ESP_LOGW(TAG, "sync_id mismatch: got %ld expected %lu",
                   (long)dresp->sync_id, (unsigned long)pending_sync_id);
          free(recv_cb->data);
          break;
        }

        int64_t rtt = t5 - pending_t3;
        int64_t one_way = rtt / 2;
        int64_t clock_offset = (pending_t1 + one_way) - pending_t2;

        ESP_LOGI(TAG, "Sync id=%lu rtt=%lld us one_way=%lld us offset=%lld us",
                 (unsigned long)pending_sync_id, rtt, one_way, clock_offset);

        led_update_t u = {
            .set_sync = true,
            .t0_us = pending_t2 - pending_t1 - one_way,
            .tx_us = t5,
        };
        update_led(u);

        if (!synced) {
          synced = true;
          led_update_t mode_u = {
              .set_mode = true,
              .mode = LED_MODE_SYNCED,
              .set_color = true,
              .color = (rgb){0, 100, 0},
          };
          update_led(mode_u);
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

void mesh_node_start(send_param_t *send_param, QueueHandle_t queue) {
  ESP_LOGI(TAG, "Starting node");

  node_task_params_t *params = malloc(sizeof(node_task_params_t));
  params->send_param = send_param;
  params->queue = queue;

  xTaskCreate(node_task, "mesh_node", 4096, params, 4, NULL);
}
