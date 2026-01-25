#pragma once
#include "freertos/idf_additions.h"
#include <stdbool.h>
#include <stdint.h>

#define LED_GPIO_R GPIO_NUM_26
#define LED_GPIO_G GPIO_NUM_25
#define LED_GPIO_B GPIO_NUM_27

#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_HIGH_SPEED_MODE
#define LEDC_CHANNEL_R LEDC_CHANNEL_0
#define LEDC_CHANNEL_G LEDC_CHANNEL_1
#define LEDC_CHANNEL_B LEDC_CHANNEL_2
#define LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define LEDC_DUTY_MAX 255

static QueueHandle_t led_queue;

typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
} __attribute__((packed)) rgb;

typedef enum {
  LED_MODE_SEARCHING, // red blink
  LED_MODE_SYNCED,    // green synced blink
} led_mode_t;

typedef struct {
  led_mode_t mode;
  uint32_t period_ms;
  rgb color;
} led_task_state_t;

typedef struct {
  bool set_mode;
  led_mode_t mode;
  bool set_led_on;
  bool led_on;

  bool set_period;
  uint32_t period_ms;

  bool set_color;
  rgb color;

  int64_t tx_us;

  bool set_sync;
  int64_t t0_us; // sync reference in esp_timer time
} led_update_t;

void leds_set_rgb(bool state, rgb color);
void leds_init(void);
void update_led(led_update_t u);
