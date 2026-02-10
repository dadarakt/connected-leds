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

typedef enum {
  ROOT_STATE_BROADCASTING,
  ROOT_STATE_UNICASTING,
} root_state_t;

typedef struct {
  send_param_t *send_param;
  QueueHandle_t queue;
} root_task_params_t;

static void root_task(void *pvParameter) {
  root_task_params_t *params = (root_task_params_t *)pvParameter;
  send_param_t *send_param = params->send_param;
  QueueHandle_t queue = params->queue;
  free(params);

  espnow_event_t evt;
  uint8_t recv_payload_type = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;
  int ret;

  root_state_t state = ROOT_STATE_BROADCASTING;
  uint32_t sync_id = 0;

  // Set yellow LED for root while broadcasting
  led_update_t led = {
      .set_color = true,
      .color = (rgb){100, 100, 0},
  };
  update_led(led);

  vTaskDelay(pdMS_TO_TICKS(3000));
  ESP_LOGI(TAG, "State=BROADCASTING, starting broadcast");

  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Send error");
    espnow_deinit(send_param, queue);
    vTaskDelete(NULL);
  }

  TickType_t wait_ticks = portMAX_DELAY;

  while (true) {
    BaseType_t got = xQueueReceive(queue, &evt, wait_ticks);

    if (got != pdTRUE) {
      // Timeout — only happens in UNICASTING state, send sync
      sync_msg_t sync = {.t_1 = esp_timer_get_time(), .sync_id = sync_id++};
      espnow_data_prepare_payload(send_param, PAYLOAD_TYPE_SYNC, &sync,
                                  sizeof(sync));
      if (esp_now_send(send_param->dest_mac, send_param->buffer,
                       send_param->len) != ESP_OK) {
        ESP_LOGE(TAG, "Sync send error");
      }
      ESP_LOGI(TAG, "Sent sync id=%lu t1=%lld", (unsigned long)sync.sync_id,
               sync.t_1);
      continue;
    }

    switch (evt.id) {
    case ESPNOW_SEND_CB: {
      event_send_cb_t *send_cb = &evt.info.send_cb;
      ESP_LOGD(TAG, "Sent to " MACSTR ", status: %d",
               MAC2STR(send_cb->mac_addr), send_cb->status);

      if (state == ROOT_STATE_BROADCASTING) {
        if (send_param->delay > 0) {
          vTaskDelay(pdMS_TO_TICKS(send_param->delay));
        }
        espnow_data_prepare(send_param);
        if (esp_now_send(send_param->dest_mac, send_param->buffer,
                         send_param->len) != ESP_OK) {
          ESP_LOGE(TAG, "Send error");
          espnow_deinit(send_param, queue);
          vTaskDelete(NULL);
        }
      }
      // In UNICASTING state, don't chain sends — sync is timeout-driven
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

      if (state == ROOT_STATE_BROADCASTING) {
        ESP_LOGI(TAG, "Received from node " MACSTR,
                 MAC2STR(recv_cb->mac_addr));

        if (espnow_add_peer(recv_cb->mac_addr)) {
          memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
          send_param->broadcast = false;
          send_param->unicast = true;

          state = ROOT_STATE_UNICASTING;
          wait_ticks = pdMS_TO_TICKS(5000);
          ESP_LOGI(TAG, "State=UNICASTING, connected to " MACSTR,
                   MAC2STR(recv_cb->mac_addr));

          int64_t now = esp_timer_get_time();
          led_update_t u = {
              .set_mode = true,
              .mode = LED_MODE_SYNCED,
              .set_color = true,
              .color = (rgb){0, 100, 0},
              .set_sync = true,
              .t0_us = 0,
              .tx_us = now,
          };
          update_led(u);
        }
      } else if (state == ROOT_STATE_UNICASTING &&
                 recv_payload_type == PAYLOAD_TYPE_DELAY_REQ) {
        espnow_data_t *buf = (espnow_data_t *)recv_cb->data;
        delay_request_t *dreq = (delay_request_t *)buf->payload;
        uint32_t req_sync_id = dreq->sync_id;

        delay_response_t dresp = {.t_4 = esp_timer_get_time(),
                                  .sync_id = req_sync_id};
        espnow_data_prepare_payload(send_param, PAYLOAD_TYPE_DELAY_RESP, &dresp,
                                    sizeof(dresp));
        if (esp_now_send(send_param->dest_mac, send_param->buffer,
                         send_param->len) != ESP_OK) {
          ESP_LOGE(TAG, "Delay response send error");
        }
        ESP_LOGI(TAG, "Sent delay_response id=%lu t4=%lld",
                 (unsigned long)req_sync_id, dresp.t_4);
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
