#pragma once

#define MESH_CHANNEL 6
#define MESH_MAX_LAYER 3

// Moving out config from KConfig file to here
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF WIFI_IF_STA
#define ESPNOW_PMK "pmk1234567890123"
#define ESPNOW_LMK "lmk1234567890123"
#define ESPNOW_CHANNEL 1
#define ESPNOW_SEND_COUNT 100
#define ESPNOW_SEND_DELAY 1000
#define ESPNOW_SEND_LEN 32
#define ESPNOW_ENABLE_LONG_RANGE false
#define ESPNOW_ENABLE_POWER_SAVE false
#define ESPNOW_WAKE_WINDOW 50
#define ESPNOW_WAKE_INTERVAL 100
#define ESPNOW_QUEUE_SIZE 6
#define ESPNOW_MAXDELAY 512

#define ESPNOW_SYNC_INTERVAL_MS 5000 // Root broadcasts sync this often
#define ESPNOW_DISCOVERY_INTERVAL_MS                                           \
  2000                               // Root broadcasts discovery this often
#define ESPNOW_NODE_TIMEOUT_MS 15000 // Node resets if no sync for this long
