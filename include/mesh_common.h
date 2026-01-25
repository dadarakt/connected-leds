#pragma once

#include "esp_now.h"
#include <stdint.h>

static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                                    0xFF, 0xFF, 0xFF};

// Initialize LEDs and Wi-Fi / network basics
void mesh_common_init(void);

// Set LED color
void set_led(uint8_t r, uint8_t g, uint8_t b);

// Initialize mesh stack (ESP-Mesh)
void mesh_init(int is_root);

//// Example code from
/// https://github.com/espressif/esp-idf/blob/v5.5.2/examples/wifi/espnow/main/espnow_example.h

#define IS_BROADCAST_ADDR(addr)                                                \
  (memcmp(addr, s_broadcast_mac, ESP_NOW_ETH_ALEN) == 0)

typedef enum {
  ESPNOW_SEND_CB,
  ESPNOW_RECV_CB,
} espnow_event_id_t;

typedef enum {
  PAYLOAD_TYPE_NONE,
  PAYLOAD_TYPE_LED_SYNC,
  PAYLOAD_TYPE_SYNC
} payload_type_t;

typedef struct {
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  esp_now_send_status_t status;
} event_send_cb_t;

typedef struct {
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  uint8_t *data;
  int data_len;
} event_recv_cb_t;

typedef union {
  event_send_cb_t send_cb;
  event_recv_cb_t recv_cb;
} espnow_event_info_t;

/* When ESPNOW sending or receiving callback function is called, post event to
 * ESPNOW task. */
typedef struct {
  espnow_event_id_t id;
  espnow_event_info_t info;
} espnow_event_t;

enum {
  DATA_BROADCAST,
  DATA_UNICAST,
  DATA_MAX,
};

/* User defined field of ESPNOW data in this example. */
typedef struct {
  uint8_t type;  // Broadcast or unicast ESPNOW data.
  uint8_t state; // Indicate that if has received broadcast ESPNOW data or not.
  uint16_t seq_num;   // Sequence number of ESPNOW data.
  uint16_t crc;       // CRC16 value of ESPNOW data.
  uint32_t magic;     // Magic number which is used to determine which device to
                      // send unicast ESPNOW data.
  uint8_t payload[0]; // Real payload of ESPNOW data.
} __attribute__((packed)) espnow_data_t;

/* Parameters of sending ESPNOW data. */
typedef struct {
  bool unicast;   // Send unicast ESPNOW data.
  bool broadcast; // Send broadcast ESPNOW data.
  uint8_t state;  // Indicate that if has received broadcast ESPNOW data or not.
  uint32_t magic; // Magic number which is used to determine which device to
                  // send unicast ESPNOW data.
  uint16_t delay; // Delay between sending two ESPNOW data, unit: ms.
  int len;        // Length of ESPNOW data to be sent, unit: byte.
  uint8_t *buffer;                    // Buffer pointing to ESPNOW data.
  uint8_t dest_mac[ESP_NOW_ETH_ALEN]; // MAC address of destination device.
} send_param_t;
