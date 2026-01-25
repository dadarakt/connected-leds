#pragma once
#include <stdint.h>

typedef struct {
  int64_t t_1;
  uint32_t sync_id;
} __attribute__((packed)) sync_msg_t;

typedef struct {
  uint32_t sync_id;
} __attribute__((packed)) delay_request_t;

typedef struct {
  int64_t t_4;
  int32_t sync_id;
} __attribute__((packed)) delay_response_t;
