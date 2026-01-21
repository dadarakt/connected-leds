#pragma once

#include "esp_mesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void mesh_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *event_data);
static void root_task(void *arg);
void mesh_root_start(void);
