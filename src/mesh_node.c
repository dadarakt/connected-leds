#include "mesh_node.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "mesh_common.h"
#include "mesh_config.h"
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
  uint8_t recv_state = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;
  int ret;

  node_conn_state_t state = NODE_STATE_UNCONNECTED;

  ESP_LOGI(TAG, "State=UNCONNECTED, waiting for root broadcasts");

  while (xQueueReceive(queue, &evt, portMAX_DELAY) == pdTRUE) {
    switch (evt.id) {
    case ESPNOW_SEND_CB: {
      event_send_cb_t *send_cb = &evt.info.send_cb;
      ESP_LOGD(TAG, "Sent to " MACSTR ", status: %d", MAC2STR(send_cb->mac_addr),
               send_cb->status);
      break;
    }
    case ESPNOW_RECV_CB: {
      event_recv_cb_t *recv_cb = &evt.info.recv_cb;

      ret = espnow_data_parse(recv_cb->data, recv_cb->data_len, &recv_state,
                              &recv_seq, &recv_magic);
      free(recv_cb->data);

      if (ret == DATA_BROADCAST) {
        ESP_LOGI(TAG, "Received broadcast from root " MACSTR,
                 MAC2STR(recv_cb->mac_addr));

        if (espnow_add_peer(recv_cb->mac_addr)) {
          // Send acknowledgment back to root
          memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
          espnow_data_prepare(send_param);
          esp_now_send(send_param->dest_mac, send_param->buffer,
                       send_param->len);
          ESP_LOGI(TAG, "Sent ack to root " MACSTR,
                   MAC2STR(recv_cb->mac_addr));

          // Transition to CONNECTED state with yellow LED
          state = NODE_STATE_CONNECTED;
          led_update_t u = {
              .set_color = true,
              .color = (rgb){100, 100, 0},
          };
          update_led(u);
          ESP_LOGI(TAG, "State=CONNECTED");
        }
      } else if (ret == DATA_UNICAST) {
        ESP_LOGI(TAG, "Received unicast seq=%d from root", recv_seq);
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

void mesh_node_start(send_param_t *send_param, QueueHandle_t queue) {
  ESP_LOGI(TAG, "Starting node");

  node_task_params_t *params = malloc(sizeof(node_task_params_t));
  params->send_param = send_param;
  params->queue = queue;

  xTaskCreate(node_task, "mesh_node", 2048, params, 4, NULL);
}
