
#include "driver/ledc.h"
#include "esp_crc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h" // for wifi_tx_info_t
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "leds.h"
#include "mesh_common.h"
#include "mesh_config.h"
#include "nvs_flash.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "esp_now_rgb";

typedef enum { MSG_TYPE_RGB = 0 } msg_type_t;

typedef struct {
  uint8_t type;
  uint8_t r, g, b;
  uint32_t seq; // sequence counter
} __attribute__((packed)) rgb_msg_t;

static uint32_t last_seq = 0;

// Forward declarations
void wifi_init(void);
void init_espnow(void);
void set_rgb(uint8_t r, uint8_t g, uint8_t b);
void add_broadcast_peer(void);

static QueueHandle_t s_espnow_queue = NULL;
static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                                    0xFF, 0xFF, 0xFF};
static uint16_t s_espnow_seq[ESPNOW_DATA_MAX] = {0, 0};

static void espnow_deinit(espnow_send_param_t *send_param);

// ESP-NOW callbacks

/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void espnow_send_cb(const esp_now_send_info_t *tx_info,
                           esp_now_send_status_t status) {
  espnow_event_t evt;
  espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

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
  espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
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
void espnow_data_prepare(espnow_send_param_t *send_param) {
  espnow_data_t *buf = (espnow_data_t *)send_param->buffer;

  assert(send_param->len >= sizeof(espnow_data_t));

  buf->type = IS_BROADCAST_ADDR(send_param->dest_mac) ? ESPNOW_DATA_BROADCAST
                                                      : ESPNOW_DATA_UNICAST;
  buf->state = send_param->state;
  buf->seq_num = s_espnow_seq[buf->type]++;
  buf->crc = 0;
  buf->magic = send_param->magic;
  /* Fill all remaining bytes after the data with random values */
  esp_fill_random(buf->payload, send_param->len - sizeof(espnow_data_t));
  buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

static void espnow_task(void *pvParameter) {
  espnow_event_t evt;
  uint8_t recv_state = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;
  bool is_broadcast = false;
  int ret;

  vTaskDelay(5000 / portTICK_PERIOD_MS);
  ESP_LOGI(TAG, "Start sending broadcast data");

  /* Start sending broadcast ESPNOW data. */
  espnow_send_param_t *send_param = (espnow_send_param_t *)pvParameter;
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Send error");
    espnow_deinit(send_param);
    vTaskDelete(NULL);
  }

  while (xQueueReceive(s_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
    switch (evt.id) {
    case ESPNOW_SEND_CB: {
      espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
      is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);

      ESP_LOGD(TAG, "Send data to " MACSTR ", status1: %d",
               MAC2STR(send_cb->mac_addr), send_cb->status);

      if (is_broadcast && (send_param->broadcast == false)) {
        break;
      }

      if (!is_broadcast) {
        send_param->count--;
        if (send_param->count == 0) {
          ESP_LOGI(TAG, "Send done");
          espnow_deinit(send_param);
          vTaskDelete(NULL);
        }
      }

      /* Delay a while before sending the next data. */
      if (send_param->delay > 0) {
        vTaskDelay(send_param->delay / portTICK_PERIOD_MS);
      }

      ESP_LOGI(TAG, "send data to " MACSTR "", MAC2STR(send_cb->mac_addr));

      memcpy(send_param->dest_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
      espnow_data_prepare(send_param);

      /* Send the next data after the previous data is sent. */
      if (esp_now_send(send_param->dest_mac, send_param->buffer,
                       send_param->len) != ESP_OK) {
        ESP_LOGE(TAG, "Send error");
        espnow_deinit(send_param);
        vTaskDelete(NULL);
      }
      break;
    }
    case ESPNOW_RECV_CB: {
      espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

      ret = espnow_data_parse(recv_cb->data, recv_cb->data_len, &recv_state,
                              &recv_seq, &recv_magic);
      free(recv_cb->data);
      if (ret == ESPNOW_DATA_BROADCAST) {
        ESP_LOGI(TAG, "Receive %dth broadcast data from: " MACSTR ", len: %d",
                 recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

        /* If MAC address does not exist in peer list, add it to peer list. */
        if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {
          esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
          if (peer == NULL) {
            ESP_LOGE(TAG, "Malloc peer information fail");
            espnow_deinit(send_param);
            vTaskDelete(NULL);
          }
          memset(peer, 0, sizeof(esp_now_peer_info_t));
          peer->channel = ESPNOW_CHANNEL;
          peer->ifidx = ESPNOW_WIFI_IF;
          peer->encrypt = true;
          memcpy(peer->lmk, ESPNOW_LMK, ESP_NOW_KEY_LEN);
          memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
          ESP_ERROR_CHECK(esp_now_add_peer(peer));
          free(peer);
        }

        /* Indicates that the device has received broadcast ESPNOW data. */
        if (send_param->state == 0) {
          send_param->state = 1;
        }

        /* If receive broadcast ESPNOW data which indicates that the other
         * device has received broadcast ESPNOW data and the local magic number
         * is bigger than that in the received broadcast ESPNOW data, stop
         * sending broadcast ESPNOW data and start sending unicast ESPNOW data.
         */
        if (recv_state == 1) {
          /* The device which has the bigger magic number sends ESPNOW data, the
           * other one receives ESPNOW data.
           */
          if (send_param->unicast == false && send_param->magic >= recv_magic) {
            ESP_LOGI(TAG, "Start sending unicast data");
            ESP_LOGI(TAG, "send data to " MACSTR "",
                     MAC2STR(recv_cb->mac_addr));

            /* Start sending unicast ESPNOW data. */
            memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
            espnow_data_prepare(send_param);
            if (esp_now_send(send_param->dest_mac, send_param->buffer,
                             send_param->len) != ESP_OK) {
              ESP_LOGE(TAG, "Send error");
              espnow_deinit(send_param);
              vTaskDelete(NULL);
            } else {
              send_param->broadcast = false;
              send_param->unicast = true;
            }
          }
        }
      } else if (ret == ESPNOW_DATA_UNICAST) {
        ESP_LOGI(TAG, "Receive %dth unicast data from: " MACSTR ", len: %d",
                 recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

        /* If receive unicast ESPNOW data, also stop sending broadcast ESPNOW
         * data. */
        send_param->broadcast = false;
      } else {
        ESP_LOGI(TAG, "Receive error data from: " MACSTR "",
                 MAC2STR(recv_cb->mac_addr));
      }
      break;
    }
    default:
      ESP_LOGE(TAG, "Callback type error: %d", evt.id);
      break;
    }
  }
}

static esp_err_t espnow_init(void) {
  espnow_send_param_t *send_param;

  s_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
  if (s_espnow_queue == NULL) {
    ESP_LOGE(TAG, "Create queue fail");
    return ESP_FAIL;
  }

  /* Initialize ESPNOW and register sending and receiving callback function. */
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
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
  send_param = malloc(sizeof(espnow_send_param_t));
  if (send_param == NULL) {
    ESP_LOGE(TAG, "Malloc send parameter fail");
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
    esp_now_deinit();
    return ESP_FAIL;
  }
  memset(send_param, 0, sizeof(espnow_send_param_t));
  send_param->unicast = false;
  send_param->broadcast = true;
  send_param->state = 0;
  send_param->magic = esp_random();
  send_param->count = ESPNOW_SEND_COUNT;
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
  memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
  espnow_data_prepare(send_param);

  xTaskCreate(espnow_task, "example_espnow_task", 2048, send_param, 4, NULL);

  return ESP_OK;
}

// Initialize Wi-Fi STA mode
void wifi_init(void) {
  // esp_netif_init();
  // esp_event_loop_create_default();
  // wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  // esp_wifi_init(&cfg);
  // esp_wifi_set_mode(WIFI_MODE_STA);
  // esp_wifi_start();

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

static void espnow_deinit(espnow_send_param_t *send_param) {
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

  wifi_init();
  espnow_init();
}

// void on_data_sent(const esp_now_send_info_t *info) {
//   ESP_LOGI(TAG, "ESP-NOW send status: %s",
//            info->status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
// }

// void on_data_recv(const uint8_t *mac_addr, const uint8_t *data, int len) {
//   if (len < sizeof(rgb_msg_t))
//     return;
//   const rgb_msg_t *msg = (rgb_msg_t *)data;
//
//   if (msg->type != MSG_TYPE_RGB)
//     return;
//
//   // Only update if sequence number is newer
//   if (msg->seq > last_seq) {
//     last_seq = msg->seq;
//     set_rgb(msg->r, msg->g, msg->b);
//     ESP_LOGI(TAG,
//              "Updated RGB from %02x:%02x:%02x:%02x:%02x:%02x -> R:%d G:%d
//              B:%d "
//              "(seq:%d)",
//              mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4],
//              mac_addr[5], msg->r, msg->g, msg->b, msg->seq);
//   }
// }

// Add broadcast peer
void add_broadcast_peer(void) {
  esp_now_peer_info_t peer = {0};
  memset(peer.peer_addr, 0xFF, 6); // broadcast
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

//// Initialize ESP-NOW
// void init_espnow(void) {
//   if (esp_now_init() != ESP_OK) {
//     ESP_LOGE(TAG, "ESP-NOW init failed");
//     return;
//   }
//   esp_now_register_send_cb(on_data_sent);
//   esp_now_register_recv_cb(on_data_recv);
//   add_broadcast_peer();
// }
//
//// Set RGB LED using LEDC PWM
// void set_rgb(uint8_t r, uint8_t g, uint8_t b) {
//   ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_R, r);
//   ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_R);
//
//   ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_G, g);
//   ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_G);
//
//   ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_B, b);
//   ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_B);
// }
//
//// Task: periodically generate and broadcast a color
// void rgb_broadcast_task(void *arg) {
//   rgb_msg_t msg = {0};
//   msg.type = MSG_TYPE_RGB;
//
//   uint32_t seq = 0;
//   uint8_t r = 255, g = 0, b = 0;
//
//   while (1) {
//     seq++;
//     msg.seq = seq;
//     msg.r = r;
//     msg.g = g;
//     msg.b = b;
//
//     set_rgb(r, g, b);
//     esp_now_send(NULL, (uint8_t *)&msg, sizeof(msg));
//
//     // simple rainbow cycle
//     uint8_t tmp = r;
//     r = g;
//     g = b;
//     b = tmp;
//
//     vTaskDelay(pdMS_TO_TICKS(2000)); // change color every 2 seconds
//   }
// }
//
//// LEDC PWM init
// void ledc_init(void) {
//   ledc_timer_config_t timer = {.duty_resolution = LEDC_DUTY_RES,
//                                .freq_hz = 5000,
//                                .speed_mode = LEDC_MODE,
//                                .timer_num = LEDC_TIMER};
//   ledc_timer_config(&timer);
//
//   ledc_channel_config_t channels[3] = {{.channel = LEDC_CHANNEL_R,
//                                         .duty = 0,
//                                         .gpio_num = LED_R,
//                                         .speed_mode = LEDC_MODE,
//                                         .hpoint = 0,
//                                         .timer_sel = LEDC_TIMER},
//                                        {.channel = LEDC_CHANNEL_G,
//                                         .duty = 0,
//                                         .gpio_num = LED_G,
//                                         .speed_mode = LEDC_MODE,
//                                         .hpoint = 0,
//                                         .timer_sel = LEDC_TIMER},
//                                        {.channel = LEDC_CHANNEL_B,
//                                         .duty = 0,
//                                         .gpio_num = LED_B,
//                                         .speed_mode = LEDC_MODE,
//                                         .hpoint = 0,
//                                         .timer_sel = LEDC_TIMER}};
//   for (int i = 0; i < 3; i++)
//     ledc_channel_config(&channels[i]);
// }
//
// void app_main(void) {
//   nvs_flash_init();
//   init_wifi();
//   init_espnow();
//   ledc_init();
//
//   xTaskCreate(rgb_broadcast_task, "rgb_task", 4096, NULL, 5, NULL);
//
//   ESP_LOGI(TAG, "ESP-NOW RGB sync node started");
// }
