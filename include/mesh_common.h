#pragma once

#include "role.h"
#include <stdint.h>

// Initialize LEDs and Wi-Fi / network basics
void mesh_common_init(void);

// Set LED color
void set_led(uint8_t r, uint8_t g, uint8_t b);

// Initialize mesh stack (ESP-Mesh)
void mesh_init(int is_root);
