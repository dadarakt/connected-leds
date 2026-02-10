#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "mesh_common.h"
#include "role.h"

#ifndef IS_ROOT
#error "Build must define ROLE_ROOT or ROLE_NODE"
#endif

// Synced pattern: 3 colors, each with 3 fast pulses, then a break.
// Per color group: 3x (100ms on + 100ms off) = 600ms, then 400ms break = 1000ms
// Total cycle = 3 colors x 1000ms = 3000ms
//
//    0- 100ms  red on     600- 700ms  green on    1200-1300ms  blue on
//  100- 200ms  off        700- 800ms  off         1300-1400ms  off
//  200- 300ms  red on     800- 900ms  green on    1400-1500ms  blue on
//  300- 400ms  off        900-1000ms  off         1500-1600ms  off
//  400- 500ms  red on    1000-1100ms  green on    1600-1700ms  blue on
//  500-1000ms  off       1100-1200ms  off         1700-3000ms  off (long break)
#define PATTERN_PERIOD_US 3000000
#define GROUP_DUR_US 1000000
#define PULSE_DUR_US 100000
#define PULSE_CYCLE_US 200000
#define PULSES_PER_GROUP 3
#define NUM_GROUPS 3

static const rgb group_colors[] = {
    {100, 0, 0},
    {0, 100, 0},
    {0, 0, 100},
};

static void led_task(void *pvParameter) {
  led_task_state_t state = {
      .mode = LED_MODE_SEARCHING,
      .period_ms = 500,
      .color = {.r = 100, .g = 0, .b = 0},
  };

  int64_t t0_local_us = esp_timer_get_time();
  bool has_sync = false;

  while (1) {
    led_update_t update;
    // check for incoming LED updates, implicit 20ms delay (50fps)
    if (xQueueReceive(led_queue, &update, pdMS_TO_TICKS(20)) == pdTRUE) {

      if (update.set_mode)
        state.mode = update.mode;
      if (update.set_period)
        state.period_ms = update.period_ms;
      if (update.set_color)
        state.color = update.color;

      if (update.set_sync) {
        // adjust sender t0 into local clock domain
        int64_t now_us = esp_timer_get_time();
        t0_local_us = update.t0_us + (now_us - update.tx_us);
        has_sync = true;
      }
    }

    int64_t now_us = esp_timer_get_time();

    if (state.mode == LED_MODE_SEARCHING || !has_sync) {
      // simple blink while not synced
      bool led_on = ((now_us / 500000) % 2);
      leds_set_rgb(led_on, state.color);
    } else {
      int64_t phase_us = (now_us - t0_local_us) % PATTERN_PERIOD_US;
      if (phase_us < 0)
        phase_us += PATTERN_PERIOD_US;

      int group = phase_us / GROUP_DUR_US;
      int64_t in_group = phase_us % GROUP_DUR_US;
      int pulse = in_group / PULSE_CYCLE_US;
      int64_t in_pulse = in_group % PULSE_CYCLE_US;

      bool on = group < NUM_GROUPS && pulse < PULSES_PER_GROUP &&
                in_pulse < PULSE_DUR_US;
      rgb color = group < NUM_GROUPS ? group_colors[group] : (rgb){0, 0, 0};
      leds_set_rgb(on, color);
    }
  }
}

void app_main(void) {
  leds_init();
  xTaskCreate(led_task, "led_task", 2048, NULL, 1, NULL);

  mesh_init(IS_ROOT);
}
