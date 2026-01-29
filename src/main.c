#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "mesh_common.h"
#include "role.h"

#ifndef IS_ROOT
#error "Build must define ROLE_ROOT or ROLE_NODE"
#endif

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
    int64_t period_us = (int64_t)state.period_ms * 1000;

    bool led_on;
    if (state.mode == LED_MODE_SEARCHING || !has_sync) {
      // simple blink while not synced
      led_on = ((now_us / 500000) % 2);
    } else {
      int64_t phase_us = (now_us - t0_local_us) % period_us;
      if (phase_us < 0)
        phase_us += period_us;
      led_on = (phase_us < (period_us / 2));
    }

    leds_set_rgb(led_on, state.color);
  }
}

void app_main(void) {
  leds_init();
  xTaskCreate(led_task, "led_task", 2048, NULL, 1, NULL);

  mesh_init(IS_ROOT);
}
