#include "mesh_root.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "mesh_common.h"
#include "mesh_config.h"
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
  uint8_t recv_state = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;
  int ret;

  root_state_t state = ROOT_STATE_BROADCASTING;

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

  while (xQueueReceive(queue, &evt, portMAX_DELAY) == pdTRUE) {
    switch (evt.id) {
    case ESPNOW_SEND_CB: {
      event_send_cb_t *send_cb = &evt.info.send_cb;

      ESP_LOGD(TAG, "Sent to " MACSTR ", status: %d",
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
          espnow_deinit(send_param, queue);
          vTaskDelete(NULL);
        }
      } else if (state == ROOT_STATE_UNICASTING) {
        // Send unicast to connected node
        if (esp_now_send(send_param->dest_mac, send_param->buffer,
                         send_param->len) != ESP_OK) {
          ESP_LOGE(TAG, "Send error");
          espnow_deinit(send_param, queue);
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
        ESP_LOGI(TAG, "Received %s from node " MACSTR,
                 ret == DATA_BROADCAST ? "broadcast" : "unicast",
                 MAC2STR(recv_cb->mac_addr));

        if (espnow_add_peer(recv_cb->mac_addr)) {
          // Switch to unicast to this node
          memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
          send_param->broadcast = false;
          send_param->unicast = true;

          // Transition to UNICASTING state
          state = ROOT_STATE_UNICASTING;
          ESP_LOGI(TAG, "State=UNICASTING, connected to " MACSTR,
                   MAC2STR(recv_cb->mac_addr));

          // Update LED to green for connected
          led_update_t u = {
              .set_mode = true,
              .mode = LED_MODE_SYNCED,
              .set_color = true,
              .color = (rgb){0, 100, 0},
          };
          update_led(u);
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

void mesh_root_start(send_param_t *send_param, QueueHandle_t queue) {
  ESP_LOGI(TAG, "Starting root");

  root_task_params_t *params = malloc(sizeof(root_task_params_t));
  params->send_param = send_param;
  params->queue = queue;

  xTaskCreate(root_task, "mesh_root", 2048, params, 4, NULL);
}
