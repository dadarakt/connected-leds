#include <Arduino.h>

#define LED_RED 26
#define LED_GREEN 25
#define LED_BLUE 27

long t;

void setup() {
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);

  randomSeed(12398213);
}

void set_color(int red, int green, int blue) {
  analogWrite(LED_RED, red);
  analogWrite(LED_GREEN, green);
  analogWrite(LED_BLUE, blue);
}

void set_random_color() {
  static int r, g, b;
  r = random(255);
  g = random(255);
  b = random(100);
  set_color(r, g, b);
}

void connected() {
  static long last_update;
  static uint16_t interval = 500;

  if (t - last_update > interval) {
    last_update = t;
    set_random_color();
  }
}

/**
 * Indication that the node is not part of a network
 */
void no_connection() {
  static long last_update;
  static uint16_t interval = 333;
  static bool is_on = false;

  if (t - last_update > interval) {
    last_update = t;
    is_on = !is_on;
    is_on ? set_color(50, 0, 0) : set_color(0, 0, 0);
  }
}

void server_no_client() { set_color(0, 50, 0); }

void loop() {
  t = millis();
  // no_connection();
  connected();
}
