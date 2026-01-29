#include "leds.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/queue.h"

static const char *TAG = "leds";

QueueHandle_t led_queue;

void update_led(led_update_t u) { xQueueSend(led_queue, &u, 0); }

void leds_set_rgb(bool on, rgb color) {
  uint32_t r = 0;
  uint32_t g = 0;
  uint32_t b = 0;

  if (on) {
    r = color.r;
    g = color.g;
    b = color.b;
  }

  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_R, r);
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_G, g);
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_B, b);

  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_R);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_G);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_B);
}

void leds_init(void) {
  led_queue = xQueueCreate(4, sizeof(led_update_t));

  /* Configure LEDC timer */
  ledc_timer_config_t timer_cfg = {
      .speed_mode = LEDC_MODE,
      .timer_num = LEDC_TIMER,
      .duty_resolution = LEDC_DUTY_RES,
      .freq_hz = 5000,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

  ledc_channel_config_t ch_cfg = {
      .speed_mode = LEDC_MODE,
      .timer_sel = LEDC_TIMER,
      .duty = 0,
      .hpoint = 0,
  };

  ch_cfg.channel = LEDC_CHANNEL_R;
  ch_cfg.gpio_num = LED_GPIO_R;
  ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

  ch_cfg.channel = LEDC_CHANNEL_G;
  ch_cfg.gpio_num = LED_GPIO_G;
  ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

  ch_cfg.channel = LEDC_CHANNEL_B;
  ch_cfg.gpio_num = LED_GPIO_B;
  ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

  ESP_LOGI(TAG, "LEDC RGB initialized");
}
