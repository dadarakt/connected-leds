#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mesh_common.h"

void mesh_root_start(send_param_t *send_param, QueueHandle_t queue);
